#include "grasp_tracer.h"

#include <fstream>
#include <sstream>

bool grasp_tracer::s_enabled = false;
std::string grasp_tracer::s_path;
std::vector<std::string> grasp_tracer::s_buffers;
size_t grasp_tracer::s_flush_threshold = 1u << 16;  // 64 KB per-SM buffer

std::string grasp_tracer::s_dump_path;
unsigned long long grasp_tracer::s_dump_lo = 0;
unsigned long long grasp_tracer::s_dump_hi = 0;
int grasp_tracer::s_dump_sm = -1;
unsigned long long grasp_tracer::s_dump_pc = 0;
unsigned long long grasp_tracer::s_dump_count = 0;
unsigned long long grasp_tracer::s_dump_max = 8000;  // snapshot budget

void grasp_tracer::init(bool enable, const char *path, unsigned n_sms,
                        unsigned long long dump_lo, unsigned long long dump_hi,
                        int dump_sm, unsigned long long dump_pc) {
  s_enabled = enable && path && *path;
  s_path.clear();
  s_buffers.clear();
  s_dump_path.clear();
  s_dump_lo = dump_lo;
  s_dump_hi = dump_hi;
  s_dump_sm = dump_sm;
  s_dump_pc = dump_pc;
  s_dump_count = 0;
  if (!s_enabled) return;

  s_path = path;
  s_buffers.assign(n_sms, std::string());

  std::ofstream file(s_path.c_str(), std::ios::out | std::ios::trunc);
  if (!file.good()) {
    s_enabled = false;
    s_path.clear();
    s_buffers.clear();
    return;
  }
  file << "cycle,sm_id,warp_id,event,pc,addr,detail\n";

  // Derive state-dump path and truncate it. Armed only when a non-empty
  // window [dump_lo,dump_hi) is given; otherwise dump_active() stays false.
  if (s_dump_hi > s_dump_lo) {
    s_dump_path = s_path;
    const std::string csv = ".csv";
    if (s_dump_path.size() >= csv.size() &&
        s_dump_path.compare(s_dump_path.size() - csv.size(), csv.size(),
                            csv) == 0) {
      s_dump_path.erase(s_dump_path.size() - csv.size());
    }
    s_dump_path += "_statedump.txt";
    std::ofstream df(s_dump_path.c_str(), std::ios::out | std::ios::trunc);
    if (df.good()) {
      df << "# GRASP state dump  window=[" << s_dump_lo << "," << s_dump_hi
         << ")  sm=" << s_dump_sm << "  max_snapshots=" << s_dump_max << "\n";
    } else {
      s_dump_path.clear();
    }
  }
}

void grasp_tracer::emit(unsigned sm_id, unsigned warp_id,
                        unsigned long long cycle, const char *event,
                        unsigned long long pc, unsigned long long addr,
                        const char *detail) {
  if (!s_enabled) return;
  if (sm_id >= s_buffers.size()) return;

  std::ostringstream oss;
  oss << cycle << ',' << sm_id << ',' << warp_id << ',' << event << ','
      << "0x" << std::hex << pc << ','
      << "0x" << std::hex << addr << std::dec << ','
      << (detail ? detail : "") << '\n';

  std::string &buf = s_buffers[sm_id];
  buf.append(oss.str());
  if (buf.size() >= s_flush_threshold) flush_sid(sm_id);
}

void grasp_tracer::flush_all() {
  if (!s_enabled) return;
  for (unsigned i = 0; i < static_cast<unsigned>(s_buffers.size()); ++i) {
    flush_sid(i);
  }
}

void grasp_tracer::flush_sid(unsigned sid) {
  if (!s_enabled) return;
  if (sid >= s_buffers.size()) return;
  std::string &buf = s_buffers[sid];
  if (buf.empty()) return;

  std::ofstream file(s_path.c_str(), std::ios::out | std::ios::app);
  if (!file.good()) return;

  file << buf;
  buf.clear();
}

bool grasp_tracer::dump_active(unsigned long long cycle, unsigned sm_id,
                               unsigned long long pc) {
  if (s_dump_path.empty()) return false;
  if (s_dump_hi <= s_dump_lo) return false;
  if (cycle < s_dump_lo || cycle >= s_dump_hi) return false;
  if (s_dump_sm >= 0 && static_cast<int>(sm_id) != s_dump_sm) return false;
  if (s_dump_pc != 0 && pc != s_dump_pc) return false;
  if (s_dump_count >= s_dump_max) return false;
  return true;
}

void grasp_tracer::dump_write(const std::string &text) {
  if (s_dump_path.empty()) return;
  if (s_dump_count >= s_dump_max) return;
  std::ofstream f(s_dump_path.c_str(), std::ios::out | std::ios::app);
  if (!f.good()) return;
  f << text;
  ++s_dump_count;
}
