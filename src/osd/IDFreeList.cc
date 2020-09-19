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

#include "IDFreeList.h"

#if 1
#include "osd_types.h"
#define dout_context m_cct
#define dout_subsys ceph_subsys_osd
#undef  dout_prefix
#define dout_prefix _prefix(_dout, this)

static ostream& _prefix(std::ostream *_dout, const IDFreeList *ifl)
{
   return *_dout << "IFL::(" << ifl << ") ";
}

#else
// allow running this code out ceph
#include <iostream>
#include <string>
#include <cassert>
#define dout(a)     std::cout
#define dendl       std::endl
#define ceph_assert assert
#endif

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

  dout(10) << __func__ << " start-size=" << size << dendl;
}

//----------------------------------------------
void IDFreeList::start_recovery(void) {
  dout(0) << __func__ << dendl;
  ceph_assert(!p_recovery_mode);
  p_recovery_mode = true;
}

//----------------------------------------------
void IDFreeList::finish_recovery(void) {
  ceph_assert(p_recovery_mode);
  link_empty_entries();
  p_recovery_mode = false;
  dout(0) << __func__ << dendl;
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
  dout(10) << __func__ << " key=" << key << ", id=" << id << dendl;
  ceph_assert(p_recovery_mode);
  // make sure we got enough space
  if (id >= p_size) {
    grow( (id-p_size) + MIN_GROWTH_SIZE );
  }

  // Do we have an entry for this id already ?
  if (p_id_vec[id] == USED_ID) {
    ceph_assert(p_key_vec[id] != nullptr);

    // are we using the same key?
    if (*(p_key_vec[id]) == key) {
      dout(5) << __func__ << "::id=" << id << " is already mapped to key=" << key << dendl;
      return id;
    }
    else {
      // check if we got the key in other entry
      auto itr = p_map.find(key);
      if (itr == p_map.end()) {
	dout(5) << __func__ << "::ID=" << id << " is mapped to another key=" << *(p_key_vec[id]) << " Key=" << key << " doesn't exist!" << dendl;
      }
      else {
	dout(5) << __func__ << "::ID=" << id << " is mapped to another key=" << *(p_key_vec[id]) << " Key=" << key << " exists with another id=" << itr->second << dendl;
      }
      return NULL_ID;
    }
  }
  
  auto itr = p_map.find(key);
  if (itr == p_map.end()) {
    p_id_vec[id]  = USED_ID;
    p_map   [key] = id;
    create_reverse_mapping(id , key);
    return id;
  }
  else {
    dout(5) << __func__ << "::Key exists with another id=" << itr->second << dendl;
    return itr->second;
  }
}
  
//----------------------------------------------
recycle_log_id_t IDFreeList::assign_id(const std::string &key) //throw(std::bad_alloc)
{
  auto itr = p_map.find(key);
  /* if key exists already => return the associated index */
  if (itr != p_map.end()) {
    recycle_log_id_t id = itr->second;
    ceph_assert(p_key_vec[id] == &itr->first);
    dout(10) << __func__ << " key=" << key << "already exists mapped to id=" << id << dendl;
    return id;
  }
  
  if (p_size == 0) {
    grow(MIN_GROWTH_SIZE);
  } else if (p_free_count == 0) {
    ceph_assert(p_head == NULL_ID);
    ceph_assert(p_tail == NULL_ID);
    grow(std::max(MIN_GROWTH_SIZE,p_size));
  }

  ceph_assert(p_head != NULL_ID);
  ceph_assert(p_free_count > 0);
      
  recycle_log_id_t id = p_head;
  p_head       = p_id_vec[p_head];
  ceph_assert(p_id_vec[id] != USED_ID);
  p_id_vec[id] = USED_ID;
      
  p_free_count --;
  ceph_assert(p_free_count >= 0);

  p_map[key] = id;

  dout(10) << __func__ << " key=" << key << ", id=" << id << dendl;
  create_reverse_mapping(id , key);
  if (p_head == NULL_ID) {
    p_tail = NULL_ID;
    ceph_assert(p_free_count == 0);
    dout(10) << __func__ << "::Space was fully consumed!!" << dendl;
  }
  return id;
}
 
//----------------------------------------------
recycle_log_id_t IDFreeList::release_id(const std::string &key) {
  recycle_log_id_t id;
  auto itr = p_map.find(key);
  if (itr != p_map.end()) {
    id = itr->second;
    dout(10) << __func__ << " key=" << key << ", id=" << id << dendl;
    ceph_assert(p_free_count < p_size);
  } else {
    dout(10) << __func__ << " key=" << key << " **Does Not Exist**" << dendl;
    ceph_assert(p_free_count <= p_size);
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
    ceph_assert(p_tail == NULL_ID);
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
  dout(1) << __func__ << "::head=" << p_head << " p_id_vec[p_head]" << p_id_vec[p_head] << " free=" << p_free_count << dendl;
  
  p_free_count = 0;
  // find the first unused entry and assign it to p_head
  for (p_head = 0; p_head < p_size; p_head++) {
    if (p_id_vec[p_head] != USED_ID) {
      p_free_count++;
      break;
    }
  }
  
  // if all entries are used return here
  if (p_free_count == 0) {
    p_head = p_tail = NULL_ID;
    dout(1) << __func__ << "::No Free entries!!; size=" << p_size << ", free_count=" << p_free_count << ", p_head=" << p_head << dendl;
    scrub();
    return;
  }

  // then link all unused entries to it
  recycle_log_id_t prev = p_head;
  for (recycle_log_id_t next = prev + 1; next < p_size; next++) {
    if (p_id_vec[next] != USED_ID ) {
      p_free_count ++;
      p_id_vec[prev] = next;
      prev = next;
    }
  }

  // finally assign the last unused entry to p_tail
  p_tail = prev;
  p_id_vec[p_tail] = NULL_ID;
  dout(1) << __func__ << "::size=" << p_size << ", free_count=" << p_free_count << ", p_head=" << p_head << dendl;
  scrub();
}

//----------------------------------------------
void IDFreeList::grow(size_t size_to_grow) //throw(std::bad_alloc)
{
  dout(1) << __func__ << "::size=" << p_size << ", free_count=" << p_free_count << ", size_to_grow=" << size_to_grow << dendl;
  dout(1) << __func__ << "::Head=" << p_head << ", Tail=" << p_tail << dendl;
  ceph_assert(p_free_count == 0 || p_recovery_mode);
  
  recycle_log_id_t old_size = p_size;
  p_size += size_to_grow;
  //p_map.reserve(p_size);
  p_id_vec.resize(p_size, NULL_ID);
  p_key_vec.resize(p_size, nullptr);

  if (p_recovery_mode) {
    // we will call link_empty_entries() when we finish recovery
    return;
  }

  // all entries were used -> assign the first new entry to p_head
  ceph_assert(p_head == NULL_ID);
  p_head = old_size;
  
  ceph_assert(p_tail == NULL_ID);
    
  for (recycle_log_id_t i = old_size; i < p_size - 1; i++) {
    p_id_vec [i] = i + 1;
  }
  p_tail            = p_size - 1;
  p_id_vec [p_tail] = NULL_ID;
  p_key_vec[p_tail] = nullptr;
  p_free_count += size_to_grow;

  scrub();
}

//----------------------------------------------
void IDFreeList::scrub() {
  dout(1) << "SCRUBBING IFL: size=" << p_size << ", free_count=" << p_free_count << dendl;
  dout(1) << "SCRUBBING IFL: id_vec.size()=" << p_id_vec.size() << ", key_vec.size()=" << p_key_vec.size() << ", map.size()=" << p_map.size() << dendl;

  //dout(1) << "(1)SCRUBBING IFL: Scrub MAP" << dendl;
  int i = 0;
  for (auto itr = p_map.begin(); itr != p_map.end(); itr++, i++) {
    const std::string & sr = itr->first;
    recycle_log_id_t    id = itr->second;
    //dout(1) << "[" << i << "]SCRUBBING IFL: key=" << sr << " id=" << id << dendl;
    ceph_assert(p_key_vec[id] == &sr);
    ceph_assert(p_id_vec[id]  == USED_ID);
  }

  //dout(1) << "(2)SCRUBBING IFL: Scrub KEY-VEC" << dendl;
  recycle_log_id_t id = 0;
  for (auto itr = p_key_vec.begin(); itr != p_key_vec.end(); itr++, id++) {
    if (*itr == nullptr) {
      ceph_assert(p_id_vec[id] != USED_ID);
    }
    else {
      ceph_assert(p_id_vec[id] == USED_ID);
      
    }
  }

  //dout(1) << "(3)SCRUBBING IFL: Scrub ID-VEC" << dendl;
  int32_t free_count = 0, used_count = 0, count_null_id = 0;
  id = 0;
  for (auto itr = p_id_vec.begin(); itr != p_id_vec.end(); itr++, id++) {
    if (*itr == USED_ID) {
      ceph_assert(p_key_vec[id] != nullptr);
      auto itr = p_map.find(*(p_key_vec[id]));
      const std::string & sr     = itr->first;
      recycle_log_id_t    map_id = itr->second;
      ceph_assert(p_key_vec[id] == &sr);
      ceph_assert(map_id == id);
      used_count++;
    }
    else if(*itr == NULL_ID) {
      ceph_assert(p_key_vec[id] == nullptr);
      count_null_id++;
    }
    else {
      free_count++;
    }
  }

  dout(1) << "free_count=" << free_count << ", count_null_id=" << count_null_id << ", used_count="<<used_count<<", p_free_count=" << p_free_count << dendl;
  ceph_assert(free_count + count_null_id == p_free_count);
  ceph_assert(p_size - p_free_count == used_count);
  ceph_assert(count_null_id <= 1);

  //dout(1) << "(4)SCRUBBING IFL: Scrub free-list" << dendl;
  free_count = 0;
  auto head = p_head;
  for ( ; head != p_tail ; free_count++,  head = p_id_vec[head] ) {
    //dout(1) << "free_count=" << free_count << " p_id_vec[" << head << "]=" << p_id_vec[head] << dendl;
    ceph_assert(p_id_vec[head] != USED_ID);
  }

  if( p_id_vec[head] == NULL_ID ) {
    dout(1) << "free_count=" << free_count << " p_id_vec[" << head << "]=" << p_id_vec[head] << dendl;
    free_count++;
  }
  
  dout(1) << "free_count=" << free_count << ", p_free_count=" << p_free_count << " p_id_vec[head]=" << p_id_vec[head] << dendl;
  ceph_assert(free_count == p_free_count);
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
