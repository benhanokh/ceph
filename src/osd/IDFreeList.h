// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
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
#include "include/ceph_assert.h"

typedef int32_t recycle_log_id_t;
typedef std::unordered_map<std::string, recycle_log_id_t> recycle_logmap_t;

class IDFreeList {
public:
  CephContext*                    m_cct;

  IDFreeList(recycle_log_id_t size, CephContext *cct);

  void start_recovery(void);
  void finish_recovery(void);
  recycle_log_id_t map_key(const std::string &key);
  const std::string* reverse_map_id(recycle_log_id_t id);
  recycle_log_id_t assign_id(recycle_log_id_t id, const std::string &key);
  recycle_log_id_t assign_id(const std::string &key);
  recycle_log_id_t release_id(const std::string &key);

  inline recycle_logmap_t::const_iterator begin(void) const noexcept {
    return p_map.begin();
  }

  inline recycle_logmap_t::const_iterator end(void) const noexcept {
    return p_map.end();
  }

  inline const recycle_log_id_t null_id(void) {
    return NULL_ID;
  }

  inline const unsigned max_recycle_id_length(void) {
    return MAX_RECYCLE_ID_LENGTH;
  }

private:
  IDFreeList(const IDFreeList&) = delete;
  IDFreeList& operator= (const IDFreeList &other) = delete;
  
  void link_empty_entries(void);
  void grow(unsigned size_to_grow);
  void create_reverse_mapping(recycle_log_id_t id, const std::string &key);
  
  const recycle_log_id_t          NULL_ID               = -1; // all free entries should have NULL_ID value
  const recycle_log_id_t          USED_ID               = -2; // used (assigned) entries have USED_ID value
  const recycle_log_id_t          MIN_GROWTH_SIZE       = 16;
  // uint32_t allows for 10 digits numbers adding the "_dup" prefix we got 14 characters
  const unsigned                  MAX_RECYCLE_ID_LENGTH = 16;

  recycle_logmap_t                p_map;           // map from key-name to a recycle_log_id 
  std::vector<recycle_log_id_t>   p_id_vec;        // freelist of recycle_log_id_t elements
  std::vector<const std::string*> p_key_vec;       // reverse mapping from a recycle_log_id to key-name
  
  recycle_log_id_t                p_size;          // The number of elements we can currently store 
  recycle_log_id_t                p_head;          // The first unassigned recycle_log_id
  recycle_log_id_t                p_tail;          // The last  unassigned recycle_log_id
  recycle_log_id_t                p_free_count;    // The number of unused recycle_log_id entities
  bool                            p_recovery_mode; // an indication that we are recovering keys from disk
};
