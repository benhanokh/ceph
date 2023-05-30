// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Author: Gabriel BenHanokh <gbenhano@redhat.com>
 *
 * This is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1, as published by the Free Software
 * Foundation.  See file COPYING.
 *
 */
#pragma once
#include <cstdint>
#include <iostream>
#include <string>
#include <cstring>
#include <cmath>
#include <iomanip>


#include <list>
#include <map>
#include <mutex>
#include "include/buffer.h"
#include "include/ceph_assert.h"

using rx_buffer_t = std::unique_ptr<ceph::buffer::ptr_node, ceph::buffer::ptr_node::disposer>;
struct BufferCacheEntry {
  rx_buffer_t rx_buffer;
  bool        valid = false;
};

struct BufferCacheStat {
  uint64_t alloc_from_cache;
  uint64_t alloc_new;

  uint64_t free_to_cache;
  uint64_t free_dispose;

  uint64_t popback_success;
  uint64_t popback_failure;

  uint64_t invalid_skipped;
  uint64_t ref_skipped;

  BufferCacheStat() {
    alloc_from_cache = 0;
    alloc_new        = 0;
    free_to_cache    = 0;
    free_dispose     = 0;
    popback_success  = 0;
    popback_failure  = 0;
    invalid_skipped  = 0;
    ref_skipped      = 0;
  }

  friend std::ostream& operator<<(std::ostream& out, const BufferCacheStat& stats) {
    out << "alloc [" << stats.alloc_from_cache << "/" << stats.alloc_from_cache + stats.alloc_new << "] "
	<< "free [" << stats.free_to_cache    << "/" << stats.free_to_cache + stats.free_dispose << "] "
	<< "popback [" << stats.popback_success  << "/" << stats.popback_success + stats.popback_failure << "]";
    if( stats.invalid_skipped) {
      out << " invalid_skipped=" << stats.invalid_skipped ;
    }
    if( stats.ref_skipped) {
      out << " ref_skipped=" << stats.ref_skipped ;
    }

    return out;
  }
};

class BufferCache {
public:
  BufferCache(CephContext *_cct);
  ~BufferCache();

  int free_rx(rx_buffer_t && rx_buffer);
  rx_buffer_t&& alloc_rx(unsigned alloc_size, unsigned align);

  unsigned entries_count() const { return m_entries_count;}
  BufferCacheStat* get_stats() { return &m_stat;}
private:
  int  __add_entry_locked(unsigned idx, rx_buffer_t && rx_buffer);
  rx_buffer_t&& alloc_cache_entry_locked(unsigned idx);
  rx_buffer_t&& alloc_new_entry(unsigned alloc_size, unsigned align);
  void update_skip_entry_stats(unsigned idx);

  constexpr static uint32_t ARR_SIZE = 256;
  CephContext       *cct;
  std::array<BufferCacheEntry, ARR_SIZE> m_arr;
  std::mutex         m_lock;
  uint32_t           m_entries_count = 0;
  uint32_t           m_producer_idx  = 0;
  uint32_t           m_consumer_idx  = 0;
  BufferCacheStat    m_stat;
  rx_buffer_t        rx_alloc_buffer;
};


