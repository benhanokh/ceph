// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */

#include "SnapMapper.h"

#define dout_context cct
#define dout_subsys ceph_subsys_osd
#undef dout_prefix
#define dout_prefix *_dout << "snap_mapper."

using std::make_pair;
using std::map;
using std::pair;
using std::set;
using std::string;
using std::vector;

using ceph::bufferlist;
using ceph::decode;
using ceph::encode;
using ceph::timespan_str;

const string SnapMapper::LEGACY_MAPPING_PREFIX = "MAP_";
const string SnapMapper::MAPPING_PREFIX = "SNA_";
const string SnapMapper::OBJECT_PREFIX = "OBJ_";

const char *SnapMapper::PURGED_SNAP_PREFIX = "PSN_";

/*

  We have a bidirectional mapping, (1) from each snap+obj to object,
  sorted by snapshot, such that we can enumerate to identify all clones
  mapped to a particular snapshot, and (2) from object to snaps, so we
  can identify which reverse mappings exist for any given object (and,
  e.g., clean up on deletion).

  "MAP_"
  + ("%016x" % snapid)
  + "_"
  + (".%x" % shard_id)
  + "_"
  + hobject_t::to_str() ("%llx.%8x.%lx.name...." % pool, hash, snap)
  -> SnapMapping::Mapping { snap, hoid }

  "SNA_"
  + ("%lld" % poolid)
  + "_"
  + ("%016x" % snapid)
  + "_"
  + (".%x" % shard_id)
  + "_"
  + hobject_t::to_str() ("%llx.%8x.%lx.name...." % pool, hash, snap)
  -> SnapMapping::Mapping { snap, hoid }

  "OBJ_" +
  + (".%x" % shard_id)
  + hobject_t::to_str()
   -> SnapMapper::object_snaps { oid, set<snapid_t> }

  */

int OSDriver::get_keys(
  const std::set<std::string> &keys,
  std::map<std::string, bufferlist> *out)
{
  return os->omap_get_values(ch, hoid, keys, out);
}

int OSDriver::get_next(
  const std::string &key,
  pair<std::string, bufferlist> *next)
{
  ObjectMap::ObjectMapIterator iter =
    os->get_omap_iterator(ch, hoid);
  if (!iter) {
    ceph_abort();
    return -EINVAL;
  }
  iter->upper_bound(key);
  if (iter->valid()) {
    if (next)
      *next = make_pair(iter->key(), iter->value());
    return 0;
  } else {
    return -ENOENT;
  }
}

string SnapMapper::get_prefix(int64_t pool, snapid_t snap)
{
  char buf[100];
  int len = snprintf(
    buf, sizeof(buf),
    "%lld_%.*X_",
    (long long)pool,
    (int)(sizeof(snap)*2), static_cast<unsigned>(snap));
  return MAPPING_PREFIX + string(buf, len);
}

string SnapMapper::to_raw_key(
  const pair<snapid_t, hobject_t> &in)
{
  return get_prefix(in.second.pool, in.first) + shard_prefix + in.second.to_str();
}

pair<string, bufferlist> SnapMapper::to_raw(
  const pair<snapid_t, hobject_t> &in)
{
  bufferlist bl;
  encode(Mapping(in), bl);
  return make_pair(
    to_raw_key(in),
    bl);
}

pair<snapid_t, hobject_t> SnapMapper::from_raw(
  const pair<std::string, bufferlist> &image)
{
  using ceph::decode;
  Mapping map;
  bufferlist bl(image.second);
  auto bp = bl.cbegin();
  decode(map, bp);
  return make_pair(map.snap, map.hoid);
}

bool SnapMapper::is_mapping(const string &to_test)
{
  return to_test.substr(0, MAPPING_PREFIX.size()) == MAPPING_PREFIX;
}

string SnapMapper::to_object_key(const hobject_t &hoid)
{
  return OBJECT_PREFIX + shard_prefix + hoid.to_str();
}

void SnapMapper::object_snaps::encode(bufferlist &bl) const
{
  ENCODE_START(1, 1, bl);
  encode(oid, bl);
  encode(snaps, bl);
  ENCODE_FINISH(bl);
}

void SnapMapper::object_snaps::decode(bufferlist::const_iterator &bl)
{
  DECODE_START(1, bl);
  decode(oid, bl);
  decode(snaps, bl);
  DECODE_FINISH(bl);
}

bool SnapMapper::check(const hobject_t &hoid) const
{
  if (hoid.match(mask_bits, match)) {
    return true;
  }
  derr << __func__ << " " << hoid << " mask_bits " << mask_bits
       << " match 0x" << std::hex << match << std::dec << " is false"
       << dendl;
  return false;
}

void SnapMapper::print_snaps(const char *s) const
{
  for (auto itr = snap_to_objs.begin(); itr != snap_to_objs.end(); ++itr) {
    dout(1) << "PRN::GBH::SNAPMAP:: called from: [" << s << "] snap_id=" << itr->first << dendl;
    for (const hobject_t& coid : itr->second) {
      dout(1) << "PRN::GBH::SNAPMAP:: [" << itr->first << "] --> [" << coid << "]" << dendl;
      ceph_assert(check(coid));
    }
    dout(1) << "=========================================" << dendl;
  }
}

int SnapMapper::remove_mapping_from_snapid_to_hobject(
  const hobject_t &coid,
  const snapid_t  &snapid)
{
  dout(20) << "GBH::SNAPMAP::" << __func__ << "::snapid=" << snapid << dendl ;
  // remove the coid from the coid_set of objects modified since snap creation
  auto itr = snap_to_objs.find(snapid);
  if (itr != snap_to_objs.end()) {
    auto & obj_set = itr->second;
    ceph_assert(obj_set.erase(coid) == 1);
    //dout(1) << "+++GBH::SNAPMAP::" << __func__ << "::snapid=" << snapid << ", coid=" << coid << dendl ;
    // if was the last element in the set -> remove the mapping
    if (obj_set.empty()) {
#if 0
      utime_t start    = snap_trim_time[snapid];
      utime_t duration = ceph_clock_now() - start;
      dout(10) << "---GBH::SNAPMAP::" << __func__ << "::removed the last obj from snap " << snapid << dendl;
      dout(10) << "GBH::SNAPMAP::TIME::" << __func__ << "::" << pgid << "::snap_id=" << snapid << " duration=" << duration << "(sec)" << dendl;
#endif
      snap_to_objs.erase(snapid);
      // should we return -ENOENT here ???
    }
    return 0;
  }
  else {
    derr << __func__ << "::GBH::SNAPMAP::coid=" << coid << " is mapped to snapid=" << snapid
	 << " , but reverse mapping doesn't exist (-EINVAL)"<< dendl;
    ceph_assert(0);
    return -EINVAL;
  }
}

int SnapMapper::update_snaps(
  const hobject_t        &coid,
  const vector<snapid_t> &new_snaps,
  const vector<snapid_t> &old_snaps,
  MapCacher::Transaction<std::string, bufferlist> *t)
{
  dout(20) << "GBH::SNAPMAP::" << __func__ << "(" << coid << ") new_snaps = " << new_snaps << ", old_snaps = " << old_snaps << dendl;
  ceph_assert(check(coid));
  if (new_snaps.empty())
    return _remove_oid(coid, old_snaps);

  // remove the @coid from all coid_sets of snaps it no longer belongs to
  // (probably snaps are in the process of being removed)
  for (auto snap_itr = old_snaps.begin(); snap_itr != old_snaps.end(); ++snap_itr) {
    if (std::find(new_snaps.begin(), new_snaps.end(), *snap_itr) == new_snaps.end()) {
      //dout(1) << "---GBH::SNAPMAP::" << __func__ << "::remove mapping from snapid->obj_id :: " << *snap_itr << "::" << coid << dendl;
      remove_mapping_from_snapid_to_hobject(coid, *snap_itr);
      if (g_conf()->subsys.should_gather<ceph_subsys_osd, 20>()) {
	dout(20) << __func__ << " rm " << to_raw_key(make_pair(*snap_itr, coid)) << dendl;
      }
    }
  }

  return 0;
}

void SnapMapper::add_oid(
  const hobject_t            & coid,
  const std::vector<snapid_t>& snaps,
  MapCacher::Transaction<std::string, ceph::buffer::list> *t )
{
  dout(20) << "GBH::SNAPMAP::" << __func__ << "::(" << coid << ") -> (" << snaps << ")" << dendl;
  ceph_assert(!snaps.empty());
  ceph_assert(check(coid));

  // add the coid to the coid_set of all snaps affected by it
  for (auto snap_itr = snaps.begin(); snap_itr != snaps.end(); ++snap_itr) {
    //dout(1) << "+++GBH::SNAPMAP::" << __func__ << "::(" << *snap_itr << ") -> (" << coid << ")" <<  dendl;
    snap_to_objs[*snap_itr].insert(coid);
  }

  if (g_conf()->subsys.should_gather<ceph_subsys_osd, 20>()) {
    for (auto& i : snaps) {
      dout(20) << __func__ << " set " << i << dendl;
    }
  }
}

uint32_t SnapMapper::count_objects()
{
  uint32_t count = 0;
  for (auto itr = snap_to_objs.begin(); itr != snap_to_objs.end(); ++itr) {
    count += itr->second.size();
  }

  return count;
}

int SnapMapper::get_next_objects_to_trim(
  snapid_t           snap,
  unsigned           max,
  vector<hobject_t> *out)
{
  ceph_assert(out);
  ceph_assert(out->empty());
  ceph_assert(max > 0);
  dout(10) << "***GBH::SNAPMAP::" << __func__ << "::snap_id=" << snap << ", max=" << max << dendl;

  auto itr = snap_to_objs.find(snap);
  if (itr != snap_to_objs.end()) {
    auto & obj_set  = itr->second;
#if 0
    auto const & itr_time = snap_trim_time.find(snap);
    if (unlikely(itr_time == snap_trim_time.end())) {
      uint32_t global_count = count_objects();
      snap_trim_time[snap] = ceph_clock_now();
      dout(1) << "GBH::SNAPMAP::TIME::" << __func__ << "::" << pgid <<"::snap_id=" << snap
	      << ", count=" << itr->second.size() << ", global_count=" << global_count << dendl;
    }
#endif
    for (auto itr = obj_set.begin(); itr != obj_set.end(); ++itr) {
      const hobject_t & coid = *itr;
      //dout(1) << "GBH::SNAPMAP::" << __func__ << "::" << snap << "-->" << coid << dendl;
      ceph_assert(check(coid));
      out->push_back(coid);
      if (out->size() == max) {
	dout(10) << "GBH::SNAPMAP::" << __func__ << "::got max objects!!" << dendl;
	return 0;
      }
    }
  }
  else {
    // There is no mapping from @snap on the system
    dout(10) << "GBH::SNAPMAP::" << __func__ << "::There is no mapping for snap (-ENOENT)" << dendl;
    return -ENOENT;
  }

  if (out->size() == 0) {
    dout(10) << "GBH::SNAPMAP::" << __func__ << "::No Objects were found (-ENOENT)" << dendl;
    return -ENOENT;
  } else {
    //dout(1) << "GBH::SNAPMAP::" << __func__ << "::got " << out->size() << " objects!!" << dendl;
    return 0;
  }
}

int SnapMapper::reset()
{
  dout(1) << "GBH::SNAPMAP::" << __func__ << dendl;
  if (!is_disabled) {
    dout(1) << "GBH::SNAPMAP::" << __func__ << "::clearing all objects!!" << dendl;
    snap_to_objs.clear();
    is_disabled = true;
    return 0;
  }
  else {
    dout(1) << "GBH::SNAPMAP::" << __func__ << "::Mapper is already disabled " << dendl;
    return -1;
  }
}

int SnapMapper::_remove_oid(const hobject_t &coid, const std::vector<snapid_t> &old_snaps)
{
  dout(20) << "GBH::SNAPMAP::" << __func__ << "::" << coid << "::<" << old_snaps << ">" << dendl;
  ceph_assert(check(coid));

  // iterate over snap-set attached to this coid
  for (auto snap_itr = old_snaps.begin(); snap_itr != old_snaps.end(); ++snap_itr) {
    // remove the coid from the coid_set of objects modified since snap creation
    //dout(1) << "---GBH::SNAPMAP::" << __func__ << "::remove mapping from snapid->obj_id :: " << *snap_itr << "::" << coid << dendl;
    remove_mapping_from_snapid_to_hobject(coid, *snap_itr);

    if (g_conf()->subsys.should_gather<ceph_subsys_osd, 20>()) {
      dout(20) << __func__ << " rm " << to_raw_key(make_pair(*snap_itr, coid)) << dendl;
    }
  }

  return 0;
}

// -- purged snaps --

string SnapMapper::make_purged_snap_key(int64_t pool, snapid_t last)
{
  char k[80];
  snprintf(k, sizeof(k), "%s_%llu_%016llx", PURGED_SNAP_PREFIX,
	   (unsigned long long)pool, (unsigned long long)last);
  return k;
}

void SnapMapper::make_purged_snap_key_value(
  int64_t pool, snapid_t begin, snapid_t end, map<string,bufferlist> *m)
{
  string k = make_purged_snap_key(pool, end - 1);
  auto& v = (*m)[k];
  ceph::encode(pool, v);
  ceph::encode(begin, v);
  ceph::encode(end, v);
}

int SnapMapper::_lookup_purged_snap(
  CephContext *cct,
  ObjectStore *store,
  ObjectStore::CollectionHandle& ch,
  const ghobject_t& hoid,
  int64_t pool, snapid_t snap,
  snapid_t *begin, snapid_t *end)
{
  string k = make_purged_snap_key(pool, snap);
  auto it = store->get_omap_iterator(ch, hoid);
  it->lower_bound(k);
  if (!it->valid()) {
    dout(20) << __func__ << " pool " << pool << " snap " << snap
	     << " key '" << k << "' lower_bound not found" << dendl;
    return -ENOENT;
  }
  if (it->key().find(PURGED_SNAP_PREFIX) != 0) {
    dout(20) << __func__ << " pool " << pool << " snap " << snap
	     << " key '" << k << "' lower_bound got mismatched prefix '"
	     << it->key() << "'" << dendl;
    return -ENOENT;
  }
  bufferlist v = it->value();
  auto p = v.cbegin();
  int64_t gotpool;
  decode(gotpool, p);
  decode(*begin, p);
  decode(*end, p);
  if (snap < *begin || snap >= *end) {
    dout(20) << __func__ << " pool " << pool << " snap " << snap
	     << " found [" << *begin << "," << *end << "), no overlap" << dendl;
    return -ENOENT;
  }
  return 0;
}

void SnapMapper::record_purged_snaps(
  CephContext *cct,
  ObjectStore *store,
  ObjectStore::CollectionHandle& ch,
  ghobject_t hoid,
  ObjectStore::Transaction *t,
  map<epoch_t,mempool::osdmap::map<int64_t,snap_interval_set_t>> purged_snaps)
{
  dout(10) << __func__ << " purged_snaps " << purged_snaps << dendl;
  map<string,bufferlist> m;
  set<string> rm;
  for (auto& [epoch, bypool] : purged_snaps) {
    // index by (pool, snap)
    for (auto& [pool, snaps] : bypool) {
      for (auto i = snaps.begin();
	   i != snaps.end();
	   ++i) {
	snapid_t begin = i.get_start();
	snapid_t end = i.get_end();
	snapid_t before_begin, before_end;
	snapid_t after_begin, after_end;
	int b = _lookup_purged_snap(cct, store, ch, hoid,
				    pool, begin - 1, &before_begin, &before_end);
	int a = _lookup_purged_snap(cct, store, ch, hoid,
				    pool, end, &after_begin, &after_end);
	if (!b && !a) {
	  dout(10) << __func__
		   << " [" << begin << "," << end << ") - joins ["
		   << before_begin << "," << before_end << ") and ["
		   << after_begin << "," << after_end << ")" << dendl;
	  // erase only the begin record; we'll overwrite the end one
	  rm.insert(make_purged_snap_key(pool, before_end - 1));
	  make_purged_snap_key_value(pool, before_begin, after_end, &m);
	} else if (!b) {
	  dout(10) << __func__
		   << " [" << begin << "," << end << ") - join with earlier ["
		   << before_begin << "," << before_end << ")" << dendl;
	  rm.insert(make_purged_snap_key(pool, before_end - 1));
	  make_purged_snap_key_value(pool, before_begin, end, &m);
	} else if (!a) {
	  dout(10) << __func__
		   << " [" << begin << "," << end << ") - join with later ["
		   << after_begin << "," << after_end << ")" << dendl;
	  // overwrite after record
	  make_purged_snap_key_value(pool, begin, after_end, &m);
	} else {
	  make_purged_snap_key_value(pool, begin, end, &m);
	}
      }
    }
  }
  t->omap_rmkeys(ch->cid, hoid, rm);
  t->omap_setkeys(ch->cid, hoid, m);
  dout(10) << __func__ << " rm " << rm.size() << " keys, set " << m.size()
	   << " keys" << dendl;
}


bool SnapMapper::Scrubber::_parse_p()
{
  if (!psit->valid()) {
    pool = -1;
    return false;
  }
  if (psit->key().find(PURGED_SNAP_PREFIX) != 0) {
    pool = -1;
    return false;
  }
  bufferlist v = psit->value();
  auto p = v.cbegin();
  ceph::decode(pool, p);
  ceph::decode(begin, p);
  ceph::decode(end, p);
  dout(20) << __func__ << " purged_snaps pool " << pool
	   << " [" << begin << "," << end << ")" << dendl;
  psit->next();
  return true;
}

bool SnapMapper::Scrubber::_parse_m()
{
  if (!mapit->valid()) {
    return false;
  }
  if (mapit->key().find(MAPPING_PREFIX) != 0) {
    return false;
  }
  auto v = mapit->value();
  auto p = v.cbegin();
  mapping.decode(p);

  {
    unsigned long long p, s;
    long sh;
    string k = mapit->key();
    int r = sscanf(k.c_str(), "SNA_%lld_%llx.%lx", &p, &s, &sh);
    if (r != 1) {
      shard = shard_id_t::NO_SHARD;
    } else {
      shard = shard_id_t(sh);
    }
  }
  dout(20) << __func__ << " mapping pool " << mapping.hoid.pool
	   << " snap " << mapping.snap
	   << " shard " << shard
	   << " " << mapping.hoid << dendl;
  mapit->next();
  return true;
}

void SnapMapper::Scrubber::run()
{
  dout(10) << __func__ << dendl;

  psit = store->get_omap_iterator(ch, purged_snaps_hoid);
  psit->upper_bound(PURGED_SNAP_PREFIX);
  _parse_p();

  mapit = store->get_omap_iterator(ch, mapping_hoid);
  mapit->upper_bound(MAPPING_PREFIX);

  while (_parse_m()) {
    // advance to next purged_snaps range?
    while (pool >= 0 &&
	   (mapping.hoid.pool > pool ||
	    (mapping.hoid.pool == pool && mapping.snap >= end))) {
      _parse_p();
    }
    if (pool < 0) {
      dout(10) << __func__ << " passed final purged_snaps interval, rest ok"
	       << dendl;
      break;
    }
    if (mapping.hoid.pool < pool ||
	mapping.snap < begin) {
      // ok
      dout(20) << __func__ << " ok " << mapping.hoid
	       << " snap " << mapping.snap
	       << " precedes pool " << pool
	       << " purged_snaps [" << begin << "," << end << ")" << dendl;
    } else {
      assert(mapping.snap >= begin &&
	     mapping.snap < end &&
	     mapping.hoid.pool == pool);
      // invalid
      dout(10) << __func__ << " stray " << mapping.hoid
	       << " snap " << mapping.snap
	       << " in pool " << pool
	       << " shard " << shard
	       << " purged_snaps [" << begin << "," << end << ")" << dendl;
      stray.emplace_back(std::tuple<int64_t,snapid_t,uint32_t,shard_id_t>(
			   pool, mapping.snap, mapping.hoid.get_hash(),
			   shard
			   ));
    }
  }

  dout(10) << __func__ << " end, found " << stray.size() << " stray" << dendl;
  psit = ObjectMap::ObjectMapIterator();
  mapit = ObjectMap::ObjectMapIterator();
}


// -------------------------------------
// legacy conversion/support

string SnapMapper::get_legacy_prefix(snapid_t snap)
{
  char buf[100];
  int len = snprintf(
    buf, sizeof(buf),
    "%.*X_",
    (int)(sizeof(snap)*2), static_cast<unsigned>(snap));
  return LEGACY_MAPPING_PREFIX + string(buf, len);
}

string SnapMapper::to_legacy_raw_key(
  const pair<snapid_t, hobject_t> &in)
{
  return get_legacy_prefix(in.first) + shard_prefix + in.second.to_str();
}

bool SnapMapper::is_legacy_mapping(const string &to_test)
{
  return to_test.substr(0, LEGACY_MAPPING_PREFIX.size()) ==
    LEGACY_MAPPING_PREFIX;
}

int SnapMapper::convert_legacy(
  CephContext *cct,
  ObjectStore *store,
  ObjectStore::CollectionHandle& ch,
  ghobject_t hoid,
  unsigned max)
{
  uint64_t n = 0;

  ObjectMap::ObjectMapIterator iter = store->get_omap_iterator(ch, hoid);
  if (!iter) {
    return -EIO;
  }

  auto start = ceph::mono_clock::now();

  iter->upper_bound(SnapMapper::LEGACY_MAPPING_PREFIX);
  map<string,bufferlist> to_set;
  while (iter->valid()) {
    bool valid = SnapMapper::is_legacy_mapping(iter->key());
    if (valid) {
      SnapMapper::Mapping m;
      bufferlist bl(iter->value());
      auto bp = bl.cbegin();
      decode(m, bp);
      to_set.emplace(
	SnapMapper::get_prefix(m.hoid.pool, m.snap),
	bl);
      ++n;
      iter->next();
    }
    if (!valid || !iter->valid() || to_set.size() >= max) {
      ObjectStore::Transaction t;
      t.omap_setkeys(ch->cid, hoid, to_set);
      int r = store->queue_transaction(ch, std::move(t));
      ceph_assert(r == 0);
      to_set.clear();
      if (!valid) {
	break;
      }
      dout(10) << __func__ << " converted " << n << " keys" << dendl;
    }
  }

  auto end = ceph::mono_clock::now();

  dout(1) << __func__ << " converted " << n << " keys in "
	  << timespan_str(end - start) << dendl;

  // remove the old keys
  {
    ObjectStore::Transaction t;
    string end = SnapMapper::LEGACY_MAPPING_PREFIX;
    ++end[end.size()-1]; // turn _ to whatever comes after _
    t.omap_rmkeyrange(ch->cid, hoid,
		      SnapMapper::LEGACY_MAPPING_PREFIX,
		      end);
    int r = store->queue_transaction(ch, std::move(t));
    ceph_assert(r == 0);
  }
  return 0;
}
