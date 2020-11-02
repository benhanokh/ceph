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
#include <atomic>
#include <ctime>
#include <unistd.h>
#include <sys/time.h>
#if 1
#include "../rocksdb/include/rocksdb/listener.h"
#include "include/ceph_assert.h"
#include "../common/Formatter.h"
#else
#include <cassert>
struct CephContext;
struct DB;
#define ceph_assert assert
#define dout(a)     std::cout
#define dendl       std::endl
struct FlushJobInfo {
  // the id of the column family
  uint32_t cf_id;
  // the name of the column family
  std::string cf_name;
  // the path to the newly created file
};

class EventListener {
 public:
  virtual void OnFlushBegin(DB* /*db*/,
                            const FlushJobInfo& /*flush_job_info*/) {}
};
#endif


struct rdl_time_stats {
  struct timeval last_time;
  uint32_t       max_msec;
  uint32_t       min_msec;
  uint64_t       accum_msec;
};

constexpr static unsigned FLUSH_REASON_ARR_SIZE      = static_cast<unsigned>(rocksdb::FlushReason::kErrorRecovery) + 1;
constexpr static unsigned COMPACTION_REASON_ARR_SIZE = static_cast<unsigned>(rocksdb::CompactionReason::kNumOfReasons);
constexpr static unsigned MAX_COMPACTION_STATS       = 4;

struct rdl_fcf_info {
  std::string*     cf_name;
  uint32_t         flush_cnt;
  uint64_t         num_entries;
  uint64_t         num_deletions;
  uint64_t         data_size;
  rdl_time_stats   time_stat;
  uint32_t         reason[FLUSH_REASON_ARR_SIZE];
  rocksdb::TableProperties  table_properties;
};

struct rdl_ccf_info {
  std::string*                cf_name;
  uint32_t                    compaction_cnt;
  uint32_t                    reason[COMPACTION_REASON_ARR_SIZE];
  int                         base_input_level[MAX_COMPACTION_STATS];
  int                         output_level[MAX_COMPACTION_STATS];
  rocksdb::CompactionJobStats stats[MAX_COMPACTION_STATS];
};
  
class RockDBListener : public rocksdb::EventListener {
public:
  RockDBListener(CephContext *cct);
  void OnMemTableSealed(const rocksdb::MemTableInfo& info);
  void OnCompactionBegin(rocksdb::DB* db, const rocksdb::CompactionJobInfo& ci);
  void OnCompactionCompleted(rocksdb::DB* db, const rocksdb::CompactionJobInfo& ci);
  void OnFlushCompleted(rocksdb::DB* db, const rocksdb::FlushJobInfo& fji) override;
  void OnFlushBegin(rocksdb::DB* db, const rocksdb::FlushJobInfo& fji) override;
  void show_rocksdb_stats(Formatter *formatter, const std::string_view & prefix);
  
private:
  void rdl_lock()   { while(m_lock.exchange(true, std::memory_order_acquire)); }
  void rdl_unlock() { m_lock.store(false, std::memory_order_release); }
  int  dump_flush_info_row(unsigned cf_id, char *s, unsigned n);
  void show_rocksdb_flush_stats(Formatter *f, bool short_output);
  void show_rocksdb_compaction_stats(Formatter *f, bool short_output);
  
  RockDBListener(const RockDBListener&) = delete;
  RockDBListener& operator= (const RockDBListener &other) = delete;

  std::vector<rdl_fcf_info>              m_fcf_info;
  std::vector<rdl_ccf_info>              m_ccf_info;
  CephContext*                           m_cct;
  std::atomic<bool>                      m_lock;
  
  constexpr static unsigned              CF_COUNT_MAX = 32;
};


