// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*- 
// vim: ts=8 sw=2 smarttab
/*
 * Ceph - scalable distributed file system
 *
 * Copyright (C) 2004-2006 Sage Weil <sage@newdream.net>
 * Copyright (C) 2013 Cloudwatt <libre.licensing@cloudwatt.com>
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
#include <cassert>

const int32_t ID_ARR_SIZE =  793;

class IDFreeList {
 public:

  //----------------------------------------------
  IDFreeList(void) {
    for( int32_t i = 0; i < ID_ARR_SIZE - 1; i++ ) {
      p_id_arr[i] = i + 1;
    }
    p_head           = 0; 
    p_tail           = ID_ARR_SIZE - 1;
    p_id_arr[p_tail] = NULL_ID;
    p_free_count     = ID_ARR_SIZE;
  }

  //----------------------------------------------
  int32_t assignID(void) {
    assert( p_free_count <= ID_ARR_SIZE );
    if( p_head != NULL_ID ) {
      assert( p_free_count > 0 );
      
      int32_t id   = p_head;
      p_head       = p_id_arr[p_head];
      p_id_arr[id] = USED_ID;
      
      p_free_count --;
      assert( p_free_count >= 0 );
      return id;
    }
    else {
      return NULL_ID;
    }
    
  }

  //----------------------------------------------
  void releaseID(int32_t id) {
    assert( p_free_count < ID_ARR_SIZE );
    assert( id < ID_ARR_SIZE );
    assert( p_id_arr[id] == USED_ID );

    if( p_head != NULL_ID ) {
      assert( p_free_count > 0 );
      assert( p_tail != NULL_ID );
      assert( p_id_arr[p_tail] == NULL_ID );
      
      p_id_arr[p_tail] = id;
      p_tail           = id;
    }
    else {
      assert( p_free_count == 0 );
      p_head = p_tail = id;
    }

    p_id_arr[id] = NULL_ID;
    p_free_count++;
  }

  const int32_t nullID(void ) {return NULL_ID;}
 private:
  const int32_t NULL_ID     = -1;
  const int32_t USED_ID     = -2;

  int32_t p_id_arr[ID_ARR_SIZE];
  int32_t p_head;
  int32_t p_tail;
  int32_t p_free_count;
};
