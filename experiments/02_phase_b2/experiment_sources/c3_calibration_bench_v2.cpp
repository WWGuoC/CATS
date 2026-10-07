// EDBT v4 Phase B2: C3 typed-cost calibration bench, v2 (B2-6 redo).
//
// Fixes vs the Phase B (commit B3) bench, per the B2 review:
//   1. UNIQUE working set per timed cell: one fresh mbind(MPOL_BIND) buffer
//      per (device, gamma, data_type) cell — 40 cells, no sharing of a buffer
//      between patterns. Each timed pass touches >= 1.25x the per-socket LLC
//      in DISTINCT bytes (monotonic stride-2 cells use a 2.5x-LLC buffer so
//      their touched half still exceeds the LLC; string16/point disclosed
//      below). LLC size is read from sysfs at startup and logged.
//   2. LLC scrub before EVERY timed repeat (sequential walk of a dedicated
//      1.5x-LLC cleaner buffer): repeat 0 is no longer biased by memset-warm
//      lines; all repeats measure the same cold-LLC state.
//   3. Randomized cell order: the 40 cells are shuffled with a FIXED seed
//      (mt19937_64, 0xB2C3D4E5F6071829ull) and executed in that order, so
//      device/dtype adjacency effects do not align with the systematic
//      enumeration order.
//   4. 12 retained repeats per cell (>= 10 required); every repeat's raw row
//      is written; nothing is deleted or re-run based on outcomes.
//      Dispersion/uncertainty (median, MAD, IQR, distribution-free median CI)
//      are computed by the aggregation script, never inside this bench.
//
// Model operationalization (unchanged from Phase B, documented in POLICY_SPEC):
//   cost(s,dev) = sum_gamma calib(dev,gamma,dtype) * count_gamma(s) * bpv,
//   so calibration is ns per access per byte.
//
// Disclosed limitations (not silently folded):
//   - Point vs Random: Hyrise's Point counter is a one-entry position list and
//     Random a multi-entry out-of-order list; at single-value granularity both
//     are uniform random single-value accesses, so Point uses the same stream
//     SHAPE as Random but on its own unique buffer and its own shuffled index
//     stream (separate trace columns, separate calibration cells).
//   - string16: fixed 16-byte payloads emulating pmr_string's in-row size;
//     variable-length overflow is not modeled.
//
// Build: clang++-17 -O2 -std=c++17 c3_calibration_bench_v2.cpp -o c3_calibration_bench_v2
// Run:   taskset -c <core-of-node1> ./c3_calibration_bench_v2 <out_raw_csv>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <random>
#include <string>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <vector>

// numa policy constants + raw syscall (mbind() is not exported by this
// system's libc; use the kernel syscall directly — no libnuma dependency)
#include <linux/mempolicy.h>
static long mbind_raw(void* addr, unsigned long len, int mode, const unsigned long* nodemask,
                      unsigned long maxnode, unsigned flags) {
  return syscall(SYS_mbind, addr, len, mode, nodemask, maxnode, flags);
}

namespace {

constexpr size_t kRepeats = 12;  // B2-6: >= 10 retained repeats
constexpr uint64_t kCellOrderSeed = 0xB2C3D4E5F6071829ull;
constexpr double kDistinctBytesLLCFactor = 1.25;  // timed distinct bytes >= 1.25x LLC
constexpr double kScrubLLCFactor = 1.5;           // cleaner buffer = 1.5x LLC

struct DType {
  const char* name;
  size_t bytes;
};

const DType kTypes[] = {
    {"int32", 4}, {"int64", 8}, {"float32", 4}, {"float64", 8}, {"string16", 16},
};

struct Cell {
  int node;
  const char* device;
  const char* gamma;
  const DType* type;
  size_t order_index;  // position in the shuffled execution order
};

double now_ns() {
  auto ts = timespec{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

// Per-socket LLC size in bytes from sysfs (fallback: 432 MiB, this machine's L3).
size_t read_llc_bytes() {
  const char* path = "/sys/devices/system/cpu/cpu0/cache/index3/size";
  auto* f = std::fopen(path, "r");
  if (!f) return 432ull * 1024 * 1024;
  char buf[64];
  const char* read = std::fgets(buf, sizeof(buf), f);
  std::fclose(f);
  if (!read) return 432ull * 1024 * 1024;
  double value = 0.0;
  char unit = 'K';
  if (std::sscanf(buf, "%lf%c", &value, &unit) >= 1) {
    double mul = unit == 'G' ? 1024.0 * 1024 * 1024 : (unit == 'M' ? 1024.0 * 1024 : 1024.0);
    return static_cast<size_t>(value * mul);
  }
  return 432ull * 1024 * 1024;
}

size_t round_up_2mib(size_t bytes) { return (bytes + (2ull << 20) - 1) & ~static_cast<size_t>((2ull << 20) - 1); }

void* bind_buffer(size_t bytes, int node) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  unsigned long mask = 1UL << node;
  if (mbind_raw(p, bytes, MPOL_BIND, &mask, sizeof(mask) * 8, MPOL_MF_MOVE) != 0) {
    munmap(p, bytes);
    return nullptr;
  }
  return p;
}

// Residency of ONE mapping. /proc/self/numa_maps prints one line per VMA with
// only a START address (no end), lines sorted ascending: the region
// containing `ptr` is the last line whose start <= ptr. Pages counted are
// that region's; require >= 99% on the target node. (The process keeps other
// node-bound buffers mapped — cell buffer + its node's scrubber — so a global
// fraction would be meaningless; Phase B's bench mapped one buffer at a time.)
bool verify_residency(const char* tag, void* ptr, int node) {
  auto* f = std::fopen("/proc/self/numa_maps", "r");
  if (!f) return false;
  char line[8192];
  char region_line[8192] = {0};
  const auto target_addr = reinterpret_cast<uintptr_t>(ptr);
  auto region_start = 0ull;
  while (std::fgets(line, sizeof(line), f)) {
    unsigned long long start = 0;
    if (std::sscanf(line, "%llx", &start) != 1) continue;
    if (start > target_addr) break;
    if (start >= region_start) {
      region_start = start;
      std::strncpy(region_line, line, sizeof(region_line) - 1);
    }
  }
  std::fclose(f);
  long long total = 0, target = 0;
  char target_tag[8];
  std::snprintf(target_tag, sizeof(target_tag), "N%d=", node);
  for (char* tok = std::strtok(region_line, " \n"); tok; tok = std::strtok(nullptr, " \n")) {
    if (std::strncmp(tok, "N", 1) == 0 && std::strchr(tok, '=')) {
      long long pages = std::atoll(std::strchr(tok, '=') + 1);
      total += pages;
      if (std::strncmp(tok, target_tag, std::strlen(target_tag)) == 0) target += pages;
    }
  }
  const bool ok = total > 0 && static_cast<double>(target) / total >= 0.99;
  std::printf("# residency[%s]: target N%d = %lld / %lld pages (%.4f) -> %s\n", tag, node, target,
              total, total ? static_cast<double>(target) / total : 0.0, ok ? "OK" : "FAIL");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <out_raw_csv>\n", argv[0]);
    return 2;
  }
  const std::string out_path = argv[1];

  const size_t llc_bytes = read_llc_bytes();
  const size_t distinct_target = round_up_2mib(static_cast<size_t>(kDistinctBytesLLCFactor *
                                                                   static_cast<double>(llc_bytes)));
  const size_t scrub_bytes = round_up_2mib(static_cast<size_t>(kScrubLLCFactor *
                                                               static_cast<double>(llc_bytes)));
  std::printf("# llc_bytes=%zu (%.1f MiB) distinct_target=%zu scrub_bytes=%zu repeats=%zu\n",
              llc_bytes, llc_bytes / 1048576.0, distinct_target, scrub_bytes, kRepeats);

  // ── Build the 40-cell grid, then shuffle into a fixed-seed execution order ──
  auto cells = std::vector<Cell>{};
  for (int node : {1, 2}) {
    for (const auto& type : kTypes) {
      for (const char* gamma : {"sequential", "monotonic", "random", "point"}) {
        cells.push_back(Cell{node, node == 1 ? "dram" : "cxl", gamma, &type, 0});
      }
    }
  }
  {
    auto order = std::vector<size_t>(cells.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    auto rng = std::mt19937_64(kCellOrderSeed);
    std::shuffle(order.begin(), order.end(), rng);
    for (size_t position = 0; position < order.size(); ++position) {
      cells[order[position]].order_index = position;
    }
  }
  std::stable_sort(cells.begin(), cells.end(),
                   [](const Cell& a, const Cell& b) { return a.order_index < b.order_index; });

  auto* out = std::fopen(out_path.c_str(), "w");
  if (!out) {
    std::perror("fopen");
    return 2;
  }
  std::fprintf(out,
               "device,gamma,data_type,repeat,cell_order,accesses,buffer_bytes,working_set_bytes,"
               "bytes_per_value,ns_total,ns_per_access,ns_per_access_per_byte\n");

  // Dedicated LLC scrub buffer per node (bound like the cells; walked
  // sequentially before every timed repeat to evict residual lines).
  auto scrub_buffers = std::map<int, uint8_t*>{};
  for (int node : {1, 2}) {
    auto* scrub = static_cast<uint8_t*>(bind_buffer(scrub_bytes, node));
    if (!scrub) {
      std::fprintf(stderr, "scrub buffer alloc failed for node %d\n", node);
      return 2;
    }
    std::memset(scrub, 3, scrub_bytes);
    scrub_buffers[node] = scrub;
  }
  if (!verify_residency("scrub_n1", scrub_buffers[1], 1) || !verify_residency("scrub_n2", scrub_buffers[2], 2)) {
    std::fprintf(stderr, "scrub buffer residency verification failed\n");
    return 2;
  }

  long long checksum = 0;  // sink so accesses are not optimized away
  for (const auto& cell : cells) {
    // Buffer sizing (B2-6): distinct bytes touched by ONE timed pass must
    // exceed the LLC. sequential/random/point cover distinct_target bytes;
    // monotonic (stride-2, gaps) touches half its buffer, so its buffer is
    // 2x distinct_target.
    const size_t buffer_bytes =
        std::strcmp(cell.gamma, "monotonic") == 0 ? 2 * distinct_target : distinct_target;
    const size_t n_values = buffer_bytes / cell.type->bytes;
    // accesses per pass: sequential/random/point touch every value exactly
    // once (full buffer coverage); monotonic walks its whole 2x buffer with
    // stride 2 (half the values, still >= 1.25x LLC distinct bytes).
    const size_t n_access = n_values;

    auto* buf = static_cast<uint8_t*>(bind_buffer(buffer_bytes, cell.node));
    if (!buf) {
      std::fprintf(stderr, "mbind/mmap failed for node %d (%zu bytes)\n", cell.node, buffer_bytes);
      return 2;
    }
    std::memset(buf, 1, buffer_bytes);
    char tag[64];
    std::snprintf(tag, sizeof(tag), "%s_%s_%s", cell.device, cell.gamma, cell.type->name);
    if (!verify_residency(tag, buf, cell.node)) {
      std::fprintf(stderr, "residency verification failed (%s)\n", tag);
      return 2;
    }

    // Index streams for this cell's UNIQUE buffer.
    // random: full Fisher-Yates shuffle (touches every value once per pass).
    // point: same stream shape, independently seeded shuffle of the same
    //        buffer (disclosed Point==Random limitation, unique working set).
    auto indices = std::vector<size_t>(n_values);
    for (size_t i = 0; i < n_values; ++i) indices[i] = i;
    auto rng = std::mt19937_64(0xC3C3C3C3C3C3C3C3ull + cell.order_index);  // per-cell stream
    std::shuffle(indices.begin(), indices.end(), rng);
    // monotonic stream: stride 2 over a rotated window (monotone, gaps).
    auto mono = std::vector<size_t>(n_values / 2);
    for (size_t i = 0; i < mono.size(); ++i) mono[i] = (i * 2 + n_values / 4) % n_values;

    for (size_t repeat = 0; repeat < kRepeats; ++repeat) {
      // LLC scrub (not timed): evict memset/prior-repeat residual lines.
      {
        auto* scrub = scrub_buffers[cell.node];
        const size_t scrub_stride = 64;  // one cache line per step
        for (size_t off = 0; off + scrub_stride <= scrub_bytes; off += scrub_stride) {
          checksum += scrub[off];
        }
      }
      const auto t0 = now_ns();
      if (std::strcmp(cell.gamma, "sequential") == 0) {
        for (size_t k = 0; k < n_access; ++k) {
          checksum += buf[(k % n_values) * cell.type->bytes];
        }
      } else if (std::strcmp(cell.gamma, "monotonic") == 0) {
        for (size_t k = 0; k < n_access; ++k) {
          checksum += buf[mono[k % mono.size()] * cell.type->bytes];
        }
      } else {  // random and point: independent uniform random single-value streams
        for (size_t k = 0; k < n_access; ++k) {
          checksum += buf[indices[k % n_values] * cell.type->bytes];
        }
      }
      const auto t1 = now_ns();
      const double ns_total = t1 - t0;
      const double ns_per_access = ns_total / static_cast<double>(n_access);
      const size_t working_set_bytes =
          std::strcmp(cell.gamma, "monotonic") == 0 ? (n_access / 2) * cell.type->bytes
                                                    : n_access * cell.type->bytes;
      std::fprintf(out, "%s,%s,%s,%zu,%zu,%zu,%zu,%zu,%zu,%.3f,%.6f,%.9f\n", cell.device, cell.gamma,
                   cell.type->name, repeat, cell.order_index, n_access, buffer_bytes,
                   working_set_bytes, cell.type->bytes, ns_total, ns_per_access,
                   ns_per_access / static_cast<double>(cell.type->bytes));
    }
    std::fflush(out);
    std::printf("# cell done: %s (order %zu)\n", tag, cell.order_index);
    munmap(buf, buffer_bytes);
  }
  std::fclose(out);
  std::fprintf(stderr, "# checksum sink: %lld\n", checksum);
  return 0;
}
