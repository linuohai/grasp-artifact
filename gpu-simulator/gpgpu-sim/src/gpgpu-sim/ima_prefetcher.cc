#include "ima_prefetcher.h"

#include <algorithm>
#include <cassert>
#include <cstdio>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ima_prefetcher_t::ima_prefetcher_t(unsigned sm_id, unsigned ipt_size,
                                   unsigned distance, unsigned conf_thresh)
    : m_sm_id(sm_id),
      m_ipt_size(ipt_size),
      m_distance(distance),
      m_conf_thresh(conf_thresh),
      m_ipt(ipt_size) {}

// ---------------------------------------------------------------------------
// IPT lookup helpers
// ---------------------------------------------------------------------------

int ima_prefetcher_t::find_entry(new_addr_type pc) const {
  for (int i = 0; i < (int)m_ipt_size; ++i)
    if (m_ipt[i].valid && m_ipt[i].pc == pc) return i;
  return -1;
}

int ima_prefetcher_t::find_lru_victim() const {
  // Find the valid entry with the oldest last_access_time.
  // Fall back to the first invalid slot if any.
  for (int i = 0; i < (int)m_ipt_size; ++i)
    if (!m_ipt[i].valid) return i;

  int victim = 0;
  for (int i = 1; i < (int)m_ipt_size; ++i)
    if (m_ipt[i].last_access_time < m_ipt[victim].last_access_time)
      victim = i;
  return victim;
}

// ---------------------------------------------------------------------------
// Core training function
// ---------------------------------------------------------------------------
//
// Called every time a global load at `entry.pc` is observed at address
// `new_addr`. Computes the new stride (new_addr - entry.last_addr) and
// updates a 2-bit saturating confidence counter against the previously
// recorded entry.stride: on a stride match, confidence increments (capped
// at MAX_CONFIDENCE); on a mismatch (including the first-ever observation,
// where entry.stride starts at 0), the stride is recorded and confidence
// resets to 0, requiring a second matching observation before the entry is
// used to prefetch. entry.last_addr is updated unconditionally.
//
// This follows the 2-bit-confidence-per-RPT-entry scheme described in
// IMP [MICRO'15] §3.1 (match -> saturate-increment; mismatch -> reset to 0
// and record the new stride).
//
void ima_prefetcher_t::ipt_update(ipt_entry_t &entry, new_addr_type new_addr) {
  int64_t new_stride =
      static_cast<int64_t>(new_addr) - static_cast<int64_t>(entry.last_addr);
  if (entry.stride == 0) {
    entry.stride = new_stride;
    entry.confidence = 0;
  } else if (new_stride == entry.stride) {
    entry.confidence =
        std::min(entry.confidence + 1, ipt_entry_t::MAX_CONFIDENCE);
  } else {
    entry.stride = new_stride;
    entry.confidence = 0;
  }
  entry.last_addr = new_addr;
}

// ---------------------------------------------------------------------------
// Main access hook
// ---------------------------------------------------------------------------

std::vector<new_addr_type> ima_prefetcher_t::on_load_access(
    new_addr_type pc, new_addr_type addr, unsigned long long cycle) {
  m_clock = cycle;
  std::vector<new_addr_type> result;

  int idx = find_entry(pc);
  if (idx < 0) {
    // First time seeing this PC: allocate a fresh entry.
    idx = find_lru_victim();
    ipt_entry_t &e = m_ipt[idx];
    e.valid = true;
    e.pc = pc;
    e.last_addr = addr;
    e.stride = 0;
    e.confidence = 0;
    e.last_access_time = cycle;
    return result;  // No prefetch yet (need at least two observations)
  }

  ipt_entry_t &entry = m_ipt[idx];
  entry.last_access_time = cycle;

  // Train the IPT entry (stride + confidence update).
  ipt_update(entry, addr);

  // Generate prefetch addresses if confidence is sufficient and stride known.
  if (entry.stride != 0 && entry.confidence >= m_conf_thresh) {
    for (unsigned n = 1; n <= m_distance; ++n) {
      new_addr_type paddr =
          static_cast<new_addr_type>(static_cast<int64_t>(addr) +
                                     static_cast<int64_t>(n) * entry.stride);
      result.push_back(paddr);
      ++stat_prefetch_generated;
    }
  }
  return result;
}

// ---------------------------------------------------------------------------
// Statistics
// ---------------------------------------------------------------------------

void ima_prefetcher_t::print_stats(FILE *fp) const {
  fprintf(fp,
          "IMA_Prefetcher SM%u: generated=%llu issued=%llu filtered=%llu\n",
          m_sm_id, stat_prefetch_generated, stat_prefetch_issued,
          stat_prefetch_filtered);
}
