#include "buffer_cache.h"
#include "common/debug.h"

#define dout_context cct
#define dout_subsys ceph_subsys_ms
#undef dout_prefix
#define dout_prefix *_dout << __func__ << "::BCache::" << " "

//--------------------------------------------------------------------------------
BufferCache::BufferCache(CephContext *_cct)
{
  cct =_cct;
  for (unsigned i = 0; i < ARR_SIZE; i++) {
    m_arr[i].valid = false;
  }
}

//--------------------------------------------------------------------------------
BufferCache::~BufferCache()
{
  // TBD free all stored bufferlists!
  for (unsigned i = 0; i < ARR_SIZE; i++) {

    // GBH::TBD - need to find a better way to release the ptr_node
    m_arr[i].rx_buffer.reset();
  }
}

//--------------------------------------------------------------------------------
int BufferCache::__add_entry_locked(unsigned idx, rx_buffer_t && rx_buffer)
{
  //ldout(cct, 0) << "::GBH::add_entry() at index=" << i << dendl;
  m_arr[idx].valid = true;
  m_arr[idx].rx_buffer = std::move(rx_buffer);
  m_producer_idx = (idx+1) % ARR_SIZE;
  m_entries_count++;
  m_stat.free_to_cache++;
  return 0;
}

//--------------------------------------------------------------------------------
int BufferCache::free_rx(rx_buffer_t && rx_buffer)
{
  //ceph_assert(rx_buffer->raw_nref() == 1);
  // first test without lock
  if (m_entries_count >= ARR_SIZE || (rx_buffer->raw_length() % 16 != 0)) [[unlikely]] {
    ldout(cct, 0) << "::GBH::add_entry() FULL/Unaligned (unlocked)" << dendl;
    // GBH::TBD - need to find a better way to release the ptr_node
    //rx_buffer.reset();
    m_stat.free_dispose++;
    return -1;
  }

  std::unique_lock lock(m_lock); // <<<<<<
  // repeat the test under lock
  if (m_entries_count >= ARR_SIZE) [[unlikely]] {
    ldout(cct, 0) << "::GBH::add_entry() FULL (locked)" << dendl;
    // GBH::TBD - need to find a better way to release the ptr_node
    //rx_buffer.reset();
    m_stat.free_dispose++;
    return -1;
  }


  for (unsigned i = m_producer_idx; i < ARR_SIZE; i++) {
    if (m_arr[i].valid == false) {
      return __add_entry_locked(i, std::move(rx_buffer));
    }
  }

  for (unsigned i = 0; i < m_producer_idx; i++) {
    if (m_arr[i].valid == false) {
      return __add_entry_locked(i, std::move(rx_buffer));
    }
  }

  // should never happen since we checked count under lock
  ceph_abort_msg(__func__);
  return -1;
}

//--------------------------------------------------------------------------------
rx_buffer_t&& BufferCache::alloc_cache_entry_locked(unsigned idx)
{
  // ldout(cct, 0) << "::GBH::alloc() recylce entry index=" << i << dendl;
  m_arr[idx].valid = false;
  m_entries_count--;
  m_consumer_idx = (idx+1) % ARR_SIZE;
  m_stat.alloc_from_cache++;
  return std::move(m_arr[idx].rx_buffer);
}

//--------------------------------------------------------------------------------
rx_buffer_t&& BufferCache::alloc_new_entry(unsigned alloc_size, unsigned align)
{
  ldout(cct, 0) << "::BufferCache::alloc_new_entry() alloc_size=" << alloc_size << ", align=" << align << dendl;
  rx_alloc_buffer = ceph::buffer::ptr_node::create(ceph::buffer::create_aligned(alloc_size, align));
  //rx_alloc_buffer->set_length(0);
  rx_alloc_buffer->set_offset(0);

  m_stat.alloc_new++;
  return std::move(rx_alloc_buffer);
}

//--------------------------------------------------------------------------------
void BufferCache::update_skip_entry_stats(unsigned idx)
{
  if (m_arr[idx].valid == false) {
    m_stat.invalid_skipped ++;
  }
  else if (m_arr[idx].rx_buffer->raw_nref() > 1) {
    m_stat.ref_skipped ++;
  }
}

//--------------------------------------------------------------------------------
rx_buffer_t&& BufferCache::alloc_rx(unsigned alloc_size, unsigned align)
{
  // ldout(cct, 0) << "::GBH::alloc() alloc_size=" << alloc_size << dendl;
  // first test without lock
  if (m_entries_count == 0) [[unlikely]] {
    ldout(cct, 0) << "::GBH::alloc_rx() allocate new entry (unlocked)" << dendl;
    return alloc_new_entry(alloc_size, align);
  }

  std::unique_lock lock(m_lock); // <<<<<<
  // repeat the test under lock
  if (m_entries_count == 0) [[unlikely]] {
    return alloc_new_entry(alloc_size, align);
  }

  for (unsigned i = m_consumer_idx; i < ARR_SIZE; i++) {
    if (m_arr[i].valid && (m_arr[i].rx_buffer->raw_nref() == 1)) {
      return alloc_cache_entry_locked(i);
    }
    else {
      update_skip_entry_stats(i);
    }
  }

  for (unsigned i = 0; i < m_consumer_idx; i++) {
    if (m_arr[i].valid && (m_arr[i].rx_buffer->raw_nref() == 1)) {
      return alloc_cache_entry_locked(i);
    }
    else {
      update_skip_entry_stats(i);
    }
  }

  lock.unlock();
  ldout(cct, 0) << "::GBH::alloc_rx() ALLOCATE NEW ENTRY busy_entries=" << m_entries_count << dendl;
  return alloc_new_entry(alloc_size, align);

  // should never happen since we checked count under lock
  ceph_abort_msg(__func__);
}
