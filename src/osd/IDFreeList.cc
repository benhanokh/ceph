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

#include "IDFreeList.h"
#include "osd_types.h"

#define dout_context m_cct
#define dout_subsys ceph_subsys_osd
#undef  dout_prefix
#define dout_prefix _prefix(_dout, this)

static ostream& _prefix(std::ostream *_dout, const IDFreeList *ifl)
{
   return *_dout << "IFL::(" << ifl << ") ";
}

//----------------------------------------------
IDFreeList::IDFreeList(recycle_log_id_t size, CephContext *cct): p_map(size), p_id_vec(size), p_key_vec(size) //throw(std::bad_alloc)
{
  p_size = size;
    
  for (recycle_log_id_t i = 0; i < p_size - 1; i++) {
    p_id_vec [i] = i + 1;
    p_key_vec[i] = nullptr;
  }
  p_head            = 0; 
  p_tail            = p_size - 1;
  p_id_vec [p_tail] = NULL_ID;
  p_key_vec[p_tail] = nullptr;
  p_free_count      = p_size;
  p_recovery_mode   = false;
  m_cct             = cct;

  dout(10) << "IFL::("<<this<<")"<< __func__ <<" size=" << size << dendl;
}

//----------------------------------------------
void IDFreeList::start_recovery(void) {
  dout(0) << "IFL::"<< __func__ << dendl;
  ceph_assert(!p_recovery_mode);
  p_recovery_mode = true;
}

//----------------------------------------------
void IDFreeList::finish_recovery(void) {
  ceph_assert(p_recovery_mode);
  link_empty_entries();
  p_recovery_mode = true;
  dout(0) << "IFL::"<< __func__ << dendl;
}

//----------------------------------------------
recycle_log_id_t IDFreeList::map_key(const std::string &key) {
  auto itr = p_map.find(key);
  if (itr != p_map.end()) {
    /* key exists already => return the associated index */
    recycle_log_id_t id = itr->second;
    ceph_assert(p_key_vec[id] == &itr->first);
    return id;
  } else {
    return NULL_ID; 
  }
}

//----------------------------------------------
const std::string* IDFreeList::reverse_map_id(recycle_log_id_t id) {
  if (id >= p_size) {
    // should never happen ...
    //ceph_assert(id < p_size); 
    return nullptr;
  } else if (p_id_vec[id] == USED_ID) {
    ceph_assert(p_key_vec[id] != nullptr);
    ceph_assert(p_map[*(p_key_vec[id])] == id);
    return p_key_vec[id];
  } else {
    ceph_assert(p_key_vec[id] == nullptr);
    return nullptr;
  }
}
  
//----------------------------------------------
// recreate the mapping from eversion_t->get_key_name() to the recycle_log_id_t
recycle_log_id_t IDFreeList::assign_id(recycle_log_id_t id, const std::string &key) //throw(std::bad_alloc)
{
  dout(10) << "IFL::("<<this<<")"<< __func__ << " id=" << id << " key="<< key<< dendl;
  ceph_assert(p_recovery_mode);
  // make sure we got enough space
  if (id >= p_size) {
    grow( (id-p_size) + MIN_GROWTH_SIZE );
  }

  ceph_assert(p_id_vec [id] != USED_ID);
  ceph_assert(p_key_vec[id] == nullptr);
  auto itr = p_map.find(key);
  ceph_assert(itr == p_map.end());

  p_id_vec[id]  = USED_ID;
  p_map   [key] = id;
  create_reverse_mapping(id , key);
  return id;
}
  
//----------------------------------------------
recycle_log_id_t IDFreeList::assign_id(const std::string &key) //throw(std::bad_alloc)
{
  auto itr = p_map.find(key);
  /* if key exists already => return the associated index */
  if (itr != p_map.end()) {
    recycle_log_id_t id = itr->second;
    ceph_assert(p_key_vec[id] == &itr->first);
    dout(10) << "IFL::("<<this<<")"<< __func__ << " key="<< key<< "already exists mapped to id=" << id<< dendl;
    return id;
  }

  if (p_size == 0) {
    grow(MIN_GROWTH_SIZE);
  } else if (p_free_count == 0) {
    grow(std::max(MIN_GROWTH_SIZE,p_size));
  }

  ceph_assert(p_head != NULL_ID);
    
  if (p_head != NULL_ID) {
    ceph_assert(p_free_count > 0);
      
    recycle_log_id_t id = p_head;
    p_head       = p_id_vec[p_head];
    ceph_assert(p_id_vec[id] != USED_ID);
    p_id_vec[id] = USED_ID;
      
    p_free_count --;
    ceph_assert(p_free_count >= 0);

    p_map[key] = id;

    dout(10) << "IFL::("<<this<<")"<< __func__ << " key="<< key<< " id=" << id<< dendl;
    create_reverse_mapping(id , key);      
    return id;
  } else {
    return NULL_ID;
  }

}
 
//----------------------------------------------
recycle_log_id_t IDFreeList::release_id(const std::string &key) {
  ceph_assert(p_free_count < p_size);
  recycle_log_id_t id;
  auto itr = p_map.find(key);
  if (itr != p_map.end()) {
    id = itr->second;
    dout(10) << "IFL(remove)::"<< __func__ << " key="<< key<< ", recycle_id=" << id << dendl;
  } else {
    dout(10) << "IFL(remove)::"<< __func__ << " key="<< key<< " **Does Not Exist**" << dendl;
    return NULL_ID;
  }
    
  ceph_assert(id < p_size);
  ceph_assert(p_id_vec[id]  == USED_ID);
  ceph_assert(p_key_vec[id] == &itr->first);
    
  if (p_head != NULL_ID) {
    ceph_assert(p_free_count > 0);
    ceph_assert(p_tail != NULL_ID);
    ceph_assert(p_id_vec[p_tail] == NULL_ID);
    p_id_vec[p_tail] = id;
    p_tail           = id;
  } else {
    ceph_assert(p_free_count == 0);
    p_head = p_tail = id;
  }

  ceph_assert(p_map.erase(key) == 1);
  p_id_vec[id]  = NULL_ID;
  p_key_vec[id] = nullptr;
  p_free_count++;
  return id;
}

// PRIVATE METHODS

// initialize array and link unused entries after recovery
//----------------------------------------------
void IDFreeList::link_empty_entries(void) {
  dout(1) << "IFL::"<< __func__ << dendl;
  p_free_count = 0;
  // find the first unused entry and assign it to p_head
  for (p_head = 0; p_head < p_size; p_head++) {
    if (p_id_vec[p_head] == NULL_ID) {
      p_free_count++;
      break;
    }
  }
    
  // if all entries are used return here
  if (p_free_count == 0) {
    p_head = p_tail = NULL_ID;
    return;
  }

  // then link all unused entries to it
  recycle_log_id_t prev = p_head;
  for (recycle_log_id_t next = prev + 1; next < p_size; next++) {
    if (p_id_vec[next] == NULL_ID) {
      p_free_count ++;
      p_id_vec[prev] = next;
      prev = next;
    }
  }

  // finally assign the last unused entry to p_tail
  p_tail = prev;
}

//----------------------------------------------
void IDFreeList::grow(unsigned size_to_grow) //throw(std::bad_alloc)
{
  dout(1) << "IFL::"<< __func__ << " size_to_grow="<< size_to_grow<< dendl;
  recycle_log_id_t old_size = p_size;
  p_size += size_to_grow;
  p_id_vec. resize(p_size);
  p_key_vec.resize(p_size);

  if (p_recovery_mode) {
    for (recycle_log_id_t i = old_size; i < p_size; i++) {
      p_key_vec[i] = nullptr;
      p_id_vec [i] = NULL_ID;
    }
    return;
  }

  // all emntries were used -> assign the first new entry to p_head
  if (p_head == NULL_ID) {
    p_head = old_size;
  }


  // first, link the old vector with the new allocated area
  if (p_tail != NULL_ID) {
    p_id_vec[p_tail] = old_size;
  }
    
  for (recycle_log_id_t i = old_size; i < p_size - 1; i++) {
    p_id_vec [i] = i + 1;
    p_key_vec[i] = nullptr;
  }
  p_tail            = p_size - 1;
  p_id_vec [p_tail] = NULL_ID;
  p_key_vec[p_tail] = nullptr;
  p_free_count += size_to_grow;    
}

//----------------------------------------------
// Maintain a reverse mapping table from recycle_log_id to the key inside the hashtable
void IDFreeList::create_reverse_mapping(recycle_log_id_t id, const std::string &key) {
  /* get a reference to the key stored in the map */
  auto itr = p_map.find(key);
  ceph_assert(itr != p_map.end());
  const std::string &sr = itr->first;
  p_key_vec[id] = &sr;
}

//-----------------------------------------------------------------------------
//                                     E  O  F
//-----------------------------------------------------------------------------
