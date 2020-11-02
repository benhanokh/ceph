#include "RockDBListener.h"
#include <algorithm>    // std::min

#include "../osd/osd_types.h"
#define dout_context m_cct
#define dout_subsys ceph_subsys_osd
#undef  dout_prefix
#define dout_prefix _prefix(_dout, this)

static ostream& _prefix(std::ostream *_dout, const RockDBListener *rdl)
{
   return *_dout << "RDL::(" << rdl << ") ";
}

using ceph::Formatter;

//-----------------------------------------------------------------
RockDBListener::RockDBListener(CephContext *cct) : m_fcf_info(CF_COUNT_MAX), m_ccf_info(CF_COUNT_MAX), m_cct(cct), m_lock(false) {
  struct timeval now;
  gettimeofday(&now, 0);

  for (unsigned i = 0; i < CF_COUNT_MAX; i++) {
    memset(&m_ccf_info[i].reason, 0, sizeof(m_ccf_info[i].reason));
    memset(&m_ccf_info[i].base_input_level, 0, sizeof(m_ccf_info[i].base_input_level));
    memset(&m_ccf_info[i].output_level, 0, sizeof(m_ccf_info[i].output_level));
    
    m_ccf_info[i].cf_name              = nullptr;
    m_ccf_info[i].compaction_cnt       = 0;


    memset(&m_fcf_info[i].reason, 0, sizeof(m_fcf_info[i].reason));
    m_fcf_info[i].cf_name              = nullptr;
    m_fcf_info[i].flush_cnt            = 0;
    m_fcf_info[i].num_entries          = 0;
    m_fcf_info[i].num_deletions        = 0;

    m_fcf_info[i].time_stat.accum_msec = 0;
    m_fcf_info[i].time_stat.max_msec   = 0;
    m_fcf_info[i].time_stat.min_msec   = 0xFFFFFFFF;
    m_fcf_info[i].time_stat.last_time  = now;
  }
}


const static char* s_flush_reason_name[] = {
  "Others",
  "GetLiveFiles",
  "ShutDown",
  "ExternalFileIngestion",
  "ManualCompaction",
  "WriteBufferManager",
  "WriteBufferFull",		// 0x06
  "Test",
  "DeleteFiles",
  "AutoCompaction",
  "ManualFlush",
  "ErrorRecovery",
  "ErrorRecoveryRetryFlush",
  "Bad Reason"
};

//-----------------------------------------------------------------
static const char* get_flush_reason_name(unsigned reason) {
  if (reason < FLUSH_REASON_ARR_SIZE) {
    return s_flush_reason_name[reason];
  }
  else {
    return s_flush_reason_name[FLUSH_REASON_ARR_SIZE];
  }
}

const static char* s_compaction_reason_name[] = {
  "Unknown",
  "[Level] number of L0 files > level0_file_num_compaction_trigger",
  "[Level] total size of level > MaxBytesForLevel()",
  "[Universal] Compacting for size amplification",
  "[Universal] Compacting for size ratio",
  "[Universal] number of sorted runs > level0_file_num_compaction_trigger",
  "[FIFO] total size > max_table_files_size",
  "[FIFO] reduce number of files",
  "[FIFO] files with creation time < (current_time - interval)",
  "Manual compaction",
  "DB::SuggestCompactRange() marked files for compaction",
  "[Level] Automatic compaction within bottommost level to cleanup duplicate versions of same user key",
  "Compaction based on TTL",
  "kFlush",
  "kExternalSstIngestion",
  "Compaction due to SST file being too old",
  "Unexpcted Reason Error"
};

//-----------------------------------------------------------------
static const char* get_compaction_reason_name(unsigned reason) {
  if (reason < COMPACTION_REASON_ARR_SIZE) {
    return s_compaction_reason_name[reason];
  }
  else {
    return s_compaction_reason_name[COMPACTION_REASON_ARR_SIZE];
  }
}

//-----------------------------------------------------------------
int RockDBListener::dump_flush_info_row(unsigned cf_id, char *s, unsigned n) {
  if (cf_id > CF_COUNT_MAX) {
    ceph_assert(cf_id < CF_COUNT_MAX);
    return -1;
  }
  rdl_fcf_info   & info   = m_fcf_info[cf_id];
  rdl_time_stats & t_stat = info.time_stat; 
  if (info.cf_name == nullptr)  return -1;

  int offset = snprintf(s, n, "[%u]%7s::flush_count=%u::time_msec{min=%u, max=%u, avg=%u}::",
			cf_id, info.cf_name->c_str(), info.flush_cnt,
			t_stat.min_msec, t_stat.max_msec, (uint32_t)(t_stat.accum_msec/info.flush_cnt));

  dout(1) << __func__ << "::" << s << dendl;
  if (offset > 0) {
    for (unsigned i = 0; i < FLUSH_REASON_ARR_SIZE; i++) {
      if (info.reason[i] == 0) {
	continue;
      }
      dout(1) << __func__ << "::[" << i << "]" << info.reason[i] << dendl;
      int n2 = snprintf(s+offset, n-offset, "(%s: %u)", get_flush_reason_name(i), info.reason[i]);
      if (n2 > 0) {
	offset += n2;
      }
      else {
	return -1;
      }
      
    }
  }

  return offset;
}

//-----------------------------------------------------------------
void RockDBListener::show_rocksdb_flush_stats(Formatter *f, bool short_output) {
  f->open_object_section("rocksdb_flush_stats");
  char buffer[1<<10];
  for (unsigned i = 0; i < CF_COUNT_MAX; i++) {
    rdl_fcf_info & info = m_fcf_info[i];
    if (info.cf_name != nullptr) {
      dout(1) << __func__ << "[" << i << "]" << info.cf_name << dendl;
      memset( buffer, 0, sizeof(buffer) );
      int n = dump_flush_info_row(i, buffer, sizeof(buffer) -1);
      if (n>0) {
	dout(1) << "dump buffer" << dendl;
	dout(1) << buffer << dendl;
	f->dump_string( "cf", buffer);
	int offset = snprintf(buffer, sizeof(buffer)-1, "[curr_data_size=%lu, avg_data_size=%lu] [curr_num_entries=%lu, avg_num_entries=%lu] [curr_num_deletions=%lu, avg_num_deletions=%lu]",
			      info.table_properties.data_size,     info.data_size/info.flush_cnt,
			      info.table_properties.num_entries,   info.num_entries/info.flush_cnt,
			      info.table_properties.num_deletions, info.num_deletions/info.flush_cnt);
	buffer[offset]='\0';
	f->dump_string( "cf", buffer);	
      }
      if (short_output == false) {
	f->dump_string("TableProperties", info.table_properties.ToString());
      }
    }
  }
  f->close_section();
}

//-----------------------------------------------------------------
void RockDBListener::show_rocksdb_compaction_stats(Formatter *f, bool short_output) {
  f->open_object_section("rocksdb_compaction_stats");
  //m_ccf_info[id].compaction_cnt;
  for (unsigned cf_id = 0; cf_id < CF_COUNT_MAX; cf_id++) {
    const rdl_ccf_info & ccf_info = m_ccf_info[cf_id];
    if (ccf_info.cf_name != nullptr) {
      dout(1) << __func__ << "[" << cf_id << "]" << *ccf_info.cf_name << ":: compaction_count=" << ccf_info.compaction_cnt << dendl;
      f->dump_unsigned("compaction_count", ccf_info.compaction_cnt);

      for (unsigned reason = 0; reason < COMPACTION_REASON_ARR_SIZE; reason++) {
	if (ccf_info.reason[reason] == 0) {
	  continue;
	}
	f->dump_unsigned(get_compaction_reason_name(reason), ccf_info.reason[reason]);
      }      
    }
    if (short_output == false) {
      char s[512];
      for (unsigned stat_idx = 0; stat_idx < MAX_COMPACTION_STATS && stat_idx < ccf_info.compaction_cnt; stat_idx++) {
	int n = snprintf(s, sizeof(s), "[%u]elapsed_micros=%lu, cpu_micros=%lu, num_input_records=%lu", stat_idx,
			 ccf_info.stats[stat_idx].elapsed_micros,
			 ccf_info.stats[stat_idx].cpu_micros,
			 ccf_info.stats[stat_idx].num_input_records);
	s[n] = '\0';
	f->dump_string("compaction_stats", s );
	f->dump_unsigned("base_input_level", ccf_info.base_input_level[stat_idx]);
	f->dump_unsigned("output_level",     ccf_info.output_level[stat_idx]);
      }
    }
  }
  
  f->close_section();
}

//-----------------------------------------------------------------
void RockDBListener::show_rocksdb_stats(Formatter *f, const std::string_view & prefix) {
  dout(1) << __func__ << "::" << prefix << dendl;
  f->dump_string( "RockDBListener::Results::", prefix );
  
  bool short_output = false;
  if (prefix == "rocksdb_stats_flush_short" ||
      prefix == "rocksdb_stats_compaction_short") {
    short_output = true;
  }

  if (prefix == "rocksdb_stats_flush" ||
      prefix == "rocksdb_stats_flush_short") {
    show_rocksdb_flush_stats(f, short_output);
  }
  else if (prefix == "rocksdb_stats_compaction"  ||
	   prefix == "rocksdb_stats_compaction_short") {
    show_rocksdb_compaction_stats(f, short_output);
  }
  else {
    f->dump_string( "RockDBListener unexpcted request", prefix );
  }
}

//-----------------------------------------------------------------
void RockDBListener::OnMemTableSealed(const rocksdb::MemTableInfo& info) {
  dout(1) << __func__  << dendl;
}

//-----------------------------------------------------------------
void RockDBListener::OnCompactionBegin(rocksdb::DB* db, const rocksdb::CompactionJobInfo& ci) {
  dout(1) << __func__ << ": cf_id=" << ci.cf_id << ", cf_name=" << ci.cf_name << dendl;
}

//-----------------------------------------------------------------
void RockDBListener::OnCompactionCompleted(rocksdb::DB* db, const rocksdb::CompactionJobInfo& ci) {
  uint32_t id = ci.cf_id;
  if (id > CF_COUNT_MAX) {
    ceph_assert(id < CF_COUNT_MAX);
    return;
  }
  dout(1) << __func__ << "::cf_id=" << id << ", cf_name=" << ci.cf_name << " count=" << m_ccf_info[id].compaction_cnt << dendl;
  dout(1) << __func__ << "::elapsed_micros=" << ci.stats.elapsed_micros << ", cpu_micros=" << ci.stats.cpu_micros << dendl;

  if (m_ccf_info[id].cf_name) {
    ceph_assert(*(m_ccf_info[id].cf_name) == ci.cf_name);
  }
  else {
    std::string* p_cf_name = new std::string(ci.cf_name);
    if (p_cf_name) {
      m_ccf_info[id].cf_name = p_cf_name;
    }
    else {
      ceph_assert(p_cf_name);
    }
  }

  if (static_cast<unsigned>(ci.compaction_reason) < COMPACTION_REASON_ARR_SIZE) {
    m_ccf_info[id].reason[static_cast<unsigned>(ci.compaction_reason)]++;
  }
  else {
    ceph_assert(static_cast<unsigned>(ci.compaction_reason) < COMPACTION_REASON_ARR_SIZE);
  }

  unsigned compaction_stat_idx = m_ccf_info[id].compaction_cnt % MAX_COMPACTION_STATS;
  m_ccf_info[id].stats[compaction_stat_idx]            = ci.stats;
  m_ccf_info[id].base_input_level[compaction_stat_idx] = ci.base_input_level;
  m_ccf_info[id].output_level[compaction_stat_idx]     = ci.output_level;

  m_ccf_info[id].compaction_cnt++;
}



//-----------------------------------------------------------------
void RockDBListener::OnFlushBegin(rocksdb::DB* db, const rocksdb::FlushJobInfo& fji)  {
  dout(1) << __func__ << ": cf_id=" << fji.cf_id << ", cf_name=" << fji.cf_name << dendl;  
}

//-----------------------------------------------------------------
void RockDBListener::OnFlushCompleted(rocksdb::DB* db, const rocksdb::FlushJobInfo& fji)  {
  dout(1) << __func__ <<  ": cf_id=" << fji.cf_id << ", cf_name=" << fji.cf_name << dendl;
  uint32_t id = fji.cf_id;
  if (id > CF_COUNT_MAX) {
    ceph_assert(id < CF_COUNT_MAX);
    return;
  }

  rdl_fcf_info   & cf_info = m_fcf_info[id];
  rdl_time_stats & t_stat  = cf_info.time_stat; 

  cf_info.flush_cnt++;

  struct timeval now;
  gettimeofday(&now, 0);
  uint32_t msec_diff = (now.tv_sec - t_stat.last_time.tv_sec) * 1000;
  msec_diff += (now.tv_usec - t_stat.last_time.tv_usec)/1000;
  t_stat.last_time = now;
  
  t_stat.accum_msec += msec_diff;
  t_stat.min_msec    = std::min(t_stat.min_msec, msec_diff);
  t_stat.max_msec    = std::max(t_stat.max_msec, msec_diff);
  
  if (cf_info.cf_name) {
    ceph_assert(*(cf_info.cf_name) == fji.cf_name);
  }
  else {
    std::string* p_cf_name = new std::string(fji.cf_name);
    if (p_cf_name) {
      cf_info.cf_name = p_cf_name;
    }
    else {
      ceph_assert(p_cf_name);
    }
  }

  if (static_cast<unsigned>(fji.flush_reason) < FLUSH_REASON_ARR_SIZE) {
    cf_info.reason[static_cast<unsigned>(fji.flush_reason)]++;
  }
  else {
    ceph_assert(static_cast<unsigned>(fji.flush_reason) < FLUSH_REASON_ARR_SIZE);
  }

  cf_info.num_entries   += fji.table_properties.num_entries;
  cf_info.num_deletions += fji.table_properties.num_deletions;
  cf_info.data_size     += fji.table_properties.data_size;
  dout(1) << "::num_entries=" << cf_info.num_entries << ", num_deletions=" << cf_info.num_deletions
	  << ", data_size=" << fji.table_properties.data_size << dendl;

  cf_info.table_properties = fji.table_properties;
  //dout(1) << "::table_properties::" << fji.table_properties.ToString() << dendl;

}

#if 0
//-----------------------------------------------------------------
void RockDBListener::OnFlushBegin(rocksdb::DB* db, const rocksdb::FlushJobInfo& fji)  {
  dout(1) << __func__ << ": cf_id=" << fji.cf_id << ", cf_name=" << fji.cf_name << dendl;
  uint32_t id = fji.cf_id;
  if (id < CF_COUNT_MAX) {
    m_flush_cnt_per_cf[id]++;
  }
  else {
    ceph_assert(id < CF_COUNT_MAX);
    return;
  }

  if (m_cf_names[id]) {
    ceph_assert(*(m_cf_names[id]) == fji.cf_name);
  }
  else {
    std::string* p_cf_name = new std::string(fji.cf_name);
    if (p_cf_name) {
      std::string *p_expected = nullptr;
      m_cf_names[id].compare_exchange_weak(p_expected, p_cf_name,
					   std::memory_order_relaxed,
					   std::memory_order_relaxed);
    }
    else {
      ceph_assert(p_cf_name);
    }
  }
}

//-----------------------------------------------------------------
void RockDBListener::ShowStats(Formatter *f) {
  dout(1) << __func__ << dendl;
  derr << "RockDBListener::Results::" << dendl;
  f->dump_unsigned( "RockDBListener::Results::", 0 );
#if 0
  f->open_object_section("rocksdb_stats");
  f->dump_stream("osd_fsid") << superdlock.osd_fsid;
  f->dump_unsigned("whoami", superdlock.whoami);
  f->dump_string("state", get_state_name(get_state()));
#endif
  
  for (unsigned i = 0; i < CF_COUNT_MAX; i++) {
    if (m_cf_names[i] != nullptr) {
      derr << "[" << i << "] CF " << *(m_cf_names[i]) << " Flush-Count=" << m_flush_cnt_per_cf[i] << dendl;
      f->dump_unsigned( *(m_cf_names[i]), m_flush_cnt_per_cf[i]);
    }

  }
}
#endif
