// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Author: Gabriel BenHanokh <benhanokh@gmail.com>
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
#include <vector>
#include <unordered_map>
#if 1
#include "include/ceph_assert.h"
#else
struct CephContext;
#endif

using recycle_log_id_t = int32_t;
using recycle_logmap_t = std::unordered_map<std::string, recycle_log_id_t>;

class IDFreeList {
public:
  CephContext*                      m_cct;

  IDFreeList(recycle_log_id_t size, CephContext *cct);

  void               start_recovery();
  void               finish_recovery();
  recycle_log_id_t   map_key(const std::string &key);
  const std::string* reverse_map_id(recycle_log_id_t id);
  recycle_log_id_t   assign_id(recycle_log_id_t id, const std::string &key);
  recycle_log_id_t   assign_id(const std::string &key);
  recycle_log_id_t   release_id(const std::string &key);
  void               scrub();

  recycle_logmap_t::const_iterator cbegin() const noexcept {
    return p_map.begin();
  }

  recycle_logmap_t::const_iterator cend() const noexcept {
    return p_map.end();
  }

  // all free entries should have NULL_ID value
  constexpr static recycle_log_id_t NULL_ID               = -1;
  // used (assigned) entries have USED_ID value
  constexpr static recycle_log_id_t USED_ID               = -2;
  // uint32_t allows for 10 digits numbers adding the "_dup" prefix we got 14 characters
  constexpr static size_t           MAX_RECYCLE_ID_LENGTH = 16;

  constexpr static recycle_log_id_t MIN_GROWTH_SIZE       = 16;
  
private:
  IDFreeList(const IDFreeList&) = delete;
  IDFreeList& operator= (const IDFreeList &other) = delete;
  
  void link_empty_entries();
  void grow(size_t size_to_grow);
  void create_reverse_mapping(recycle_log_id_t id, const std::string &key);
  
  recycle_logmap_t                  p_map;           // map from key-name to a recycle_log_id 
  std::vector<recycle_log_id_t>     p_id_vec;        // freelist of recycle_log_id_t elements
  std::vector<const std::string*>   p_key_vec;       // reverse mapping from a recycle_log_id to key-name
  
  recycle_log_id_t                  p_size;          // The number of elements we can currently store 
  recycle_log_id_t                  p_head;          // The first unassigned recycle_log_id
  recycle_log_id_t                  p_tail;          // The last  unassigned recycle_log_id
  recycle_log_id_t                  p_free_count;    // The number of unused recycle_log_id entities
  bool                              p_recovery_mode; // an indication that we are recovering keys from disk
};
