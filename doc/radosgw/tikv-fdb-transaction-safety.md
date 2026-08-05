# Transaction Safety for Object Mutations on TiKV and FDB

This document explains why naive read-then-write transaction patterns fail for
object mutations in TiKV, how the failure mode differs between version-chain
and child-key operations, why FDB is unaffected, and what the correct fix
looks like.

---

## 1. The problem

### TiKV's conflict model

TiKV transactions (Percolator model) give **snapshot isolation**, not
serializability ([TiKV deep dive: isolation levels](https://tikv.github.io/deep-dive-tikv/distributed-transaction/isolation-level.html#snapshot-isolation)):

- Reads see a consistent MVCC snapshot as of the transaction's `start_ts`.
- At commit, TiKV checks for write-write conflicts only on keys in the
  **write set** — keys you `Prewrite`. It checks whether any of those keys
  has a `commit_ts > start_ts` from a transaction that committed while yours
  was in flight.
- Keys you only read are invisible to conflict checking. A range scan gives
  **no protection** against concurrent writers inserting or modifying keys in
  that range.
- TiKV has **no gap or predicate locking** — even under pessimistic locking,
  you can only lock keys that exist. A concurrent insert into a "locked" range
  is never blocked.

### Two classes of mutation

Two classes of object mutation in RGW's KV schema expose this limitation.

**Version mutations** operate on the version chain for an object:

- `PutObject`: writes `:O:` (new current version), moves the old entry to GC.
- `DeleteCurrent` (Case 2): scans `:V:` to find the next live version, then
  writes `:O:` pointing to it and deletes the promoted `:V:` entry.
- `DeleteVersion` (Case 1): deletes a specific non-current `:V:` entry.

**Child entry mutations** read `:O:` to determine a subordinate key and write
that key:

- `PutObjectTagging` (external tags): reads `:O:` for the `ref_tag`, writes
  `:C:<ref_tag>T`.
- Annotation writes: reads `:O:` for the `ref_tag`, writes
  `:C:<ref_tag>A:<name>`.
- Extended value writes: reads `:O:` for routing, writes `:C:<ref_tag>E`.

Both classes fail naively on TiKV. The failure modes differ, and so do the
fixes.

---

## 2. Version mutations: write-set intersection works

### The technique

`DeleteCurrent` scans `:V:` to find the next version to promote, then writes
`:O:` and the promoted `:V:` entry. If a concurrent `PutObject` writes `:O:`
after the scan but before the commit, the promoted pointer would be stale.

The fix exploits a structural property of the design: **any operation that
changes what "current" points to necessarily writes `:O:`**. Two such
operations racing always collide on `:O:`, so TiKV's write-write conflict
check catches the race even though the `:V:` scan itself is unprotected.

### Worked example 1 — `PutObject` vs `DeleteCurrent`

- State: `v1(ts10)`, `v2(ts20)`, `v3(ts30)`, `:O:` → `v3`.
- Txn A (`DeleteCurrent`): `start_ts=100`. Reads `:O:`=v3. Scans `:V:`, finds
  v2. Prewrites: delete `v3`, write `:O:`=v2.
- Txn B (`PutObject` v4), concurrently: `start_ts=105`. Writes v4, writes
  `:O:`=v4. Commits at `commit_ts=110`.
- A's commit fails: `commit_ts=110 > start_ts=100` on `:O:` → `WriteConflict`.
  A must retry.
- On retry, A re-reads `:O:`=v4 and re-scans `:V:`. Finds v3 is the next to
  promote. Correct.

### Worked example 2 — `DeleteCurrent` vs `DeleteVersion` (non-current)

- State: `:O:` → v3, chain: v3 current, v2 and v1 in `:V:`.
- Txn A (`DeleteCurrent`): `start_ts=100`. Reads `:O:`=v3, scans `:V:`, finds
  v2. Prewrites: delete `:V:v2`, write `:O:`=v2. Write set:
  `{ :O:, :V:v2 }`.
- Txn B (`DeleteVersion` v2, non-current): `start_ts=105`. Reads `:V:v2`,
  prewrites: delete `:V:v2`. Write set: `{ :V:v2 }`. Commits at
  `commit_ts=110`.
- A's commit fails: `commit_ts=110 > start_ts=100` on `:V:v2` →
  `WriteConflict`. A retries.
- On retry, A re-reads `:O:`=v3 (B never touched it — still Case 2), re-scans
  `:V:` → finds only v1. Promotes v1 instead. Correct.

Here the conflict key is `:V:v2`, not `:O:`. The technique still works — A
writes the specific key it depends on — but the shared key is determined by
which version A happens to promote, not by a fixed anchor.

### Retry must recompute, not resubmit

On conflict, a transaction must re-evaluate from scratch, including
re-determining which case applies. Consider the symmetric outcome of example 2:
A commits first, making `:O:` point to v2. B retries, re-reads `:O:`=v2, which
now *matches* B's original target — B flips from Case 1 (delete non-current)
to Case 2 (delete current). B must scan `:V:` and promote v1. An
implementation that blindly re-runs its Case 1 code path on retry fails to
find v2 in `:V:` and returns a wrong 404. Recompute from scratch means
re-evaluating which case applies, not just re-executing the same case.

### Why this is structural

The write-set intersection on `:O:` is not arranged; it is **forced by
semantics**. You cannot implement `PutObject` or `DeleteCurrent` without
writing `:O:` — changing "current" is what those operations mean. Any future
operation that changes "current" will also write `:O:`, and will therefore
conflict correctly with any concurrent version mutation that depends on the
current pointer. The protection is a consequence of what the operations must
do.

---

## 3. Child entry mutations: write-set intersection has no solution

### The scenario

When object tags spill to an external child entry, a `PutObjectTagging` call
on an already-external object:

1. Reads `:O:` → `ref_tag=A`, external-tags flag set.
2. Writes `:C:A:T` with the new tags.
3. Has no legitimate write to perform on `:O:` — the object's identity,
   content, and metadata are unchanged.

Concurrent `PutObject` replaces the object: new `:O:` with `ref_tag=B`, old
`:O:` moved to GC.

Write sets:
- `PutObjectTagging`: `{ :C:A:T }`
- `PutObject`: `{ :O:, G:... }`

**No intersection.** On TiKV, `PutObjectTagging` commits successfully, writing
tags to `ref_tag=A`, which is now in GC. The GC entry was written before
`:C:A:T` existed, so the GC worker has no record of it to clean up. Tags are
silently orphaned. The new `:O:` (with `ref_tag=B`) shows no external tags.

### Why no fix exists within write-set intersection

The technique in §2 requires the *invalidating* operation to write some key
that you also write. For version mutations, `PutObject` writes `:O:`, and every
version mutation also writes `:O:` — the overlap is guaranteed by semantics.

For `PutObjectTagging`, `PutObject` writes `:O:` and `PutObjectTagging` writes
`:C:<ref_tag>T`. These are disjoint by design. There is no key that both
operations have a semantic reason to write.

One could force artificial intersection by making `PutObjectTagging`
unconditionally write `:O:` — for instance by bumping a generation counter
co-located with it. This works mechanically, but only if every future
child-key writer follows the same rule. The version-chain invariant ("does this
change 'current'? then it writes `:O:`") is **semantically verifiable** — you
can look at an operation and determine whether it applies. The child-key rule
("does this read `:O:` to route a write? then it must also write `:E:`") has
no semantic grounding; it can only be enforced by structure — which means
enforcing it through a shared entry point, not per-operation convention.

---

## 4. FDB: the problem does not arise

FDB's conflict model is the inverse of TiKV's. Where TiKV provides snapshot
isolation, FDB provides full **serializable** isolation — the strongest
isolation level ([TiKV deep dive: isolation levels](https://tikv.github.io/deep-dive-tikv/distributed-transaction/isolation-level.html#serializable)).
Every non-snapshot `Get` or `GetRange` automatically registers a
**read-conflict range** on the keys read. At commit, the Resolver checks
whether any key in those ranges was written by a transaction that committed
after the reading transaction's read version. TiKV checks write-write; FDB
checks read-write.

**For version mutations**: `GetRange` over `:V:` registers a read-conflict
range over the entire subspace. Any concurrent write into that range — a new
version insertion or a deletion — is caught. Phantom protection is free.

**For child entry mutations**: `Get(:O:)` in step 1 of `PutObjectTagging`
registers a read-conflict on `:O:`. `PutObject`'s write to `:O:` carries a
write-conflict. The Resolver fires: `PutObjectTagging` aborts and retries,
reads the new `:O:` with `ref_tag=B`, writes `:C:B:T`. Correct. No special
coordination needed — the protection is a side effect of the read that the
operation was going to perform anyway.

**Snapshot reads** (`snapshot=true` in the C API) perform a `Get` or
`GetRange` without registering a read-conflict range. They must not be used
for `:O:` reads in any mutating operation — doing so silently removes FDB's
protection.

FDB is still **fundamentally optimistic**: there is no server-side blocking.
The conflict check happens once, at commit, at the Resolver. When a transaction
is aborted, all its work is discarded. FDB degrades under sustained
high-contention on the same object (wasted retry work), while TiKV pessimistic
locking degrades more gracefully (losers queue and proceed with fresh data).

| | TiKV pessimistic lock | FDB read-conflict |
|---|---|---|
| Conflict detection | Proactive, blocking RPC at lock time | Only at commit, free |
| Loser's fate | Blocks, then proceeds with fresh data | Aborted, must redo all work |
| Uncontended cost | One extra round trip | Zero extra cost |
| Contended cost | Cheap — loser just waits | Expensive — work is wasted |
| Range/phantom protection | None | Full |
| Deadlocks | Possible; needs detection | Impossible |

FDB's ~5-second transaction limit exists because Resolvers only retain enough
recent commit history to check conflict ranges within that window.

---

## 5. Solutions

Two structural approaches make mutations safe on both backends.

**Option 1 — Declarative API**: move the abstraction boundary up. RGW
expresses *what* it wants; each backend is fully responsible for *how* to do
it safely. Version and child-key mutations, their concurrency semantics, and
conflict protection become internal backend concerns. RGW never touches the KV
primitives or the isolation model.

**Option 2 — `withObjectTxn`**: all mutations go through a shared entry point
that acquires a claim on `:O:` before the caller's logic runs. The claim
mechanism is backend-specific but hidden inside `withObjectTxn`. It is
structurally impossible to mutate any object-related key without holding the
claim.

Both are structural — the invariant is enforced by the API surface, not
per-operation convention. Option 1 is more thorough: the correctness argument
lives entirely inside the backend. Option 2 is appropriate when a full
declarative API is premature but the KV primitive layer still needs hardening.

---

## 6. Option 1: Declarative API

All versioning and child-key operations map cleanly onto a declarative
interface:

```
Put(ctx, key, value) → versionId
DeleteCurrent(ctx, key) → versionId
DeleteVersion(ctx, key, versionId) → error
GetCurrent(ctx, key) → (value, versionId)
GetVersion(ctx, key, versionId) → value
ListVersions(ctx, key, ...) → []version
PutObjectTagging(ctx, key, tags) → error
GetObjectTagging(ctx, key) → tags
DeleteObjectTagging(ctx, key) → error
```

Behind this interface, each backend owns its isolation mechanism entirely. RGW
sees none of it. The correctness argument — the entire subject of this document
— moves into the backend, which is the only place that knows its own isolation
model. Future backend authors write to the declarative contract and cannot
accidentally bypass conflict protection, because RGW never had access to the KV
primitives in the first place.

### Conditional operations

S3 conditional writes (`If-None-Match: *`, `If-Match: <etag>`) extend
naturally as richer declarative operations:

```
PutIfAbsent(ctx, key, value) → (versionId, error)
PutIfVersion(ctx, key, matchVersionId, value) → (versionId, error)
```

Each is a single backend transaction with the condition check inside. The
interface remains declarative; the backend chooses how to implement the
atomicity.

### Cross-key operations

`CopyObject` and `CompleteMultipartUpload` span more than one object. Two
options:

1. **Add them to the interface** — `CopyObject(ctx, srcKey, dstKey)`,
   `CompleteMultipartUpload(ctx, uploadId, key)`. The backend implements each
   as a single transaction where cross-key atomicity is available, or as a
   read-then-write with a version check where it is not. These are S3-shaped
   concepts, not KV primitives leaking upward.

2. **Accept the documented gap** — CopyObject at the S3 level does not
   guarantee the source is unmodified between read and write. A read-then-write
   implementation is S3-compliant; backends that can do better atomically do so
   transparently.

Exposing a transaction handle for cross-key reads would leak implementation
detail back through the interface and should be avoided.

### Testing benefit

A test suite for RGW can run against a trivial in-memory backend:

```
type MemBackend struct { mu sync.Mutex; objects map[string][]version }

func (b *MemBackend) DeleteVersion(ctx, key, versionId) error {
    b.mu.Lock(); defer b.mu.Unlock()
    // 10 lines of straightforward list manipulation
}
```

S3 operation semantics and KV transaction safety are tested independently
against their respective interfaces. Concurrency correctness is tested against
each backend's implementation, not through the full RGW stack.

---

## 7. Option 2: `withObjectTxn`

`withObjectTxn` is the sole entry point for any transaction that mutates an
object-related key. Before invoking the caller's logic it acquires a **claim
on `:O:`** — a guarantee that any concurrent `PutObject` (which always writes
`:O:`) will be detected and the caller's transaction forced to retry with
fresh state. The mechanism is backend-specific.

### FDB

On FDB, every mutating operation already reads `:O:` before making any routing
decision — to verify existence, obtain the `ref_tag`, or read the current
version. That non-snapshot read is the claim: it automatically registers a
read-conflict range on `:O:`. No extra step is needed.

```
// FDB
func withObjectTxn(txn, objectKey, fn):
    obj = txn.Get(objectKey)   // non-snapshot — registers read-conflict on :O:
    return fn(txn, obj)
```

For operations that additionally scan `:V:` or `:C:*`, FDB's `getRange` adds
read-conflict ranges over those subspaces, providing phantom protection for
free. The read must not use `snapshot=true` — that would silently remove the
protection.

### TiKV — pessimistic lock

On TiKV, a read registers nothing. The claim is an explicit `LockKeys(:O:)`:

```
// TiKV — pessimistic lock
func withObjectTxn(txn, objectKey, fn):
    txn.LockKeys(objectKey)
    return fn(txn)
```

`LockKeys` sends a `PessimisticLock` RPC that blocks if `:O:` is already
locked by a concurrent transaction. The loser waits and proceeds with fresh
data rather than discarding completed work. Conflict is detected proactively —
no wasted work on the losing side. The subsequent `Get(:O:)` to read the
object's current state is left to the caller inside `fn`; `LockKeys` alone is
sufficient for the protection.

At commit, pessimistically-locked keys go through `PrewritePessimistic`, which
skips re-checking the Write CF (already validated at lock time). Keys written
but not locked go through ordinary `Prewrite` — the standard optimistic
commit-time check. Both mechanisms can coexist within a single transaction.

Pessimistic locks can deadlock; TiKV runs deadlock detection to resolve this.

### Structural guarantee

Both implementations satisfy the same invariant: it is structurally impossible
to mutate any object-related key through `withObjectTxn` without the claim
being in place. The invariant is enforced by the API surface — `withObjectTxn`
is the only entry point — not by each operation author remembering which
protection to invoke.

The child-key problem from §3 is eliminated: `PutObjectTagging` calls
`withObjectTxn`, which acquires the claim on `:O:` before reading the
`ref_tag`. A concurrent `PutObject` is caught, forcing a retry that reads the
new `:O:` and routes to the correct child key.

---

## 8. TiKV alternative: epoch counter

Instead of a pessimistic lock, TiKV's claim can be implemented with a
monotonic generation counter co-located with `:O:` (e.g. `:E:objectKey`).

```
// TiKV — epoch counter
func withObjectTxn(txn, objectKey, fn):
    gen = txn.Get(epochKey(objectKey))
    obj = txn.Get(objectKey)
    txn.Put(epochKey(objectKey), gen + 1)
    return fn(txn, obj)
```

Every mutating operation unconditionally increments the counter. Two concurrent
mutations always have `:E:` in their write sets, so TiKV's write-write conflict
check fires, forcing one to retry. This is optimistic — no blocking RPC — so
the losing transaction's work is discarded at commit rather than queued.

The epoch write is not needed on FDB. FDB's Resolver checks read-conflict vs.
write-conflict, not write-write: writing `:E:` causes a conflict only when a
concurrent transaction has a read-conflict range covering it. The non-snapshot
`Get(:O:)` already covers `:O:` directly; the epoch key is dead weight on FDB
and can be omitted.

### Structural guarantee: same as pessimistic lock

When applied through `withObjectTxn`, the epoch counter is equally structural.
The rule "every mutation increments the counter" is enforced by the API surface
— `withObjectTxn` is the only entry point — not by per-operation convention.
This is important for child-key operations: the counter creates write-set
intersection between `PutObjectTagging` and `PutObject` even though neither has
a natural semantic reason to share a key. Applied per-operation by convention
this would be fragile; applied through `withObjectTxn` it is not.

### Benefits over pessimistic lock

- **No deadlock risk** — no locks are held, so deadlock detection is not
  needed.
- **Lower uncontended cost** — no extra round trip. The write-set check rides
  on the commit message that was being sent anyway.

### Tradeoffs

- **Late conflict detection** — conflict is discovered only at commit. If the
  losing transaction performed substantial work before committing (e.g.
  assembled a large response), all of that work is discarded and repeated. The
  pessimistic lock detects the conflict immediately and queues the loser.
- **Under sustained contention** — repeated retries discard real work rather
  than deferring it. Pessimistic locking degrades more gracefully when many
  transactions compete for the same object.

### Benchmarking required

For workloads where concurrent writes to the same object are rare (the common
case), the epoch counter's zero-cost uncontended path is likely preferable. For
workloads with sustained concurrent writes to a single object, the pessimistic
lock avoids the retry amplification. Benchmarking on realistic workloads is
required before committing to either approach for TiKV.

---

## 9. Further thoughts

### Database agnosticism and isolation level requirements

This document has analysed TiKV (snapshot isolation) and FDB (serializable)
in detail. Any additional KV backend would need to be evaluated against the
same failure modes — the correct protection mechanism depends entirely on what
isolation guarantees the database provides, and a database with weaker or
different guarantees may require yet another approach.

If the goal is complete database agnosticism — supporting arbitrary backends
without per-backend analysis — the declarative API (Option 1) is the stronger
choice. The correctness argument is fully contained inside each backend
implementation; the RGW layer is insulated from the isolation model entirely.

If agnosticism is not required and the backend set is known and fixed, the
`withObjectTxn` approach (Option 2) is viable, but the transaction safety
requirements must be documented precisely as a contract that any backend
implementation must satisfy. Without that contract, adding a new backend risks
silently introducing the races described in this document.

### Multipart uploads

The transaction safety analysis here covers version-chain mutations and
child-key mutations (tags, annotations, extended values). Multipart upload
operations — `InitiateMultipartUpload`, `UploadPart`, `CompleteMultipartUpload`,
`AbortMultipartUpload` — involve their own patterns of reads and writes across
`:M:` keys and have not been analysed in detail. It is likely that similar
transaction safety questions arise, particularly for operations that scan the
`:M:` keyspace (e.g. `AbortMultipartUpload` enumerating parts to clean up) or
that race across the initiate/complete/abort boundary. This warrants a
dedicated analysis.
