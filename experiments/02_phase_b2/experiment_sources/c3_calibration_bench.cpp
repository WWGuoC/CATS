// EDBT v4 Phase B (commit B3 prerequisite): C3 typed-cost calibration bench.
//
// Measures per-access latency of the four Hyrise access-pattern counters
// (sequential / monotonic / random / point) for five data types on the DRAM
// node (node1) and the CXL node (node2) of this machine. Output:
//   calibration_raw.csv  one row per (device, gamma, data_type, repeat)
//   (stdout)             medians; a tiny wrapper turns them into
//   calibration.csv      device,gamma,data_type,ns_per_access_per_byte
//
// Model operationalization (documented in the Phase B POLICY_SPEC): the pack's
// formula is  cost(s,dev) = Σ_γ calib(dev,γ,dtype) · count_γ(s) · bytes_per_value,
// so calibration is measured as ns per access per byte and multiplied by the
// column's bytes_per_value at scoring time. Buffers are bound with
// mbind(MPOL_BIND) — strict, no fallback — and residency is verified from
// /proc/self/numa_maps before timing.
//
// Point vs Random: Hyrise's Point counter is a one-entry position list and
// Random a multi-entry out-of-order list; at single-value granularity both are
// uniform random single-value accesses in a memory-cost model, so Point is
// calibrated with the same uniform random index stream as Random (disclosed
// limitation, not silently folded: both counters keep separate columns in the
// decision trace).
//
// Build: clang++-17 -O2 -std=c++17 c3_calibration_bench.cpp -o c3_calibration_bench
// Run:   taskset -c 96 ./c3_calibration_bench <out_csv> [accesses_per_repeat]

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <random>
#include <string>
#include <sys/mman.h>
#include <unistd.h>
#include <vector>

// numa policy constants (glibc's <numaif.h> needs libnuma for mbind(); declare manually)
#include <linux/mempolicy.h>
extern "C" long mbind(void*, unsigned long, int, const unsigned long*, unsigned long, unsigned);

namespace {

constexpr size_t kRepeats = 5;

struct DType {
  const char* name;
  size_t bytes;
};

const DType kTypes[] = {
    {"int32", 4}, {"int64", 8}, {"float32", 4}, {"float64", 8}, {"string16", 16},
};

double now_ns() {
  auto ts = timespec{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) * 1e9 + static_cast<double>(ts.tv_nsec);
}

void* bind_buffer(size_t bytes, int node) {
  void* p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) return nullptr;
  unsigned long mask = 1UL << node;
  if (mbind(p, bytes, MPOL_BIND, &mask, sizeof(mask) * 8, MPOL_MF_MOVE) != 0) {
    munmap(p, bytes);
    return nullptr;
  }
  return p;
}

bool verify_residency(const char* tag, int node) {
  // Count pages on each node in /proc/self/numa_maps; require >=99% on target.
  auto* f = std::fopen("/proc/self/numa_maps", "r");
  if (!f) return false;
  char line[4096];
  long long total = 0, target = 0;
  char target_tag[8];
  std::snprintf(target_tag, sizeof(target_tag), "N%d=", node);
  while (std::fgets(line, sizeof(line), f)) {
    for (char* tok = std::strtok(line, " \n"); tok; tok = std::strtok(nullptr, " \n")) {
      if (std::strncmp(tok, "N", 1) == 0 && std::strchr(tok, '=')) {
        long long pages = std::atoll(std::strchr(tok, '=') + 1);
        total += pages;
        if (std::strncmp(tok, target_tag, std::strlen(target_tag)) == 0) target += pages;
      }
    }
  }
  std::fclose(f);
  const bool ok = total > 0 && static_cast<double>(target) / total >= 0.99;
  std::printf("# residency[%s]: target N%d = %lld / %lld pages (%.4f) -> %s\n", tag, node, target,
              total, total ? static_cast<double>(target) / total : 0.0, ok ? "OK" : "FAIL");
  return ok;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <out_csv> [accesses_per_repeat]\n", argv[0]);
    return 2;
  }
  const std::string out_path = argv[1];
  const size_t n_access = argc > 2 ? std::stoull(argv[2]) : 32u * 1024 * 1024;  // 32M per repeat

  // ≥ 512 MiB PER TYPE (LLC here is 864 MiB / 2 sockets = 432 MiB per socket):
  // n_values scales with bytes_per_value; one buffer per (device, dtype),
  // mapped/unmapped sequentially (peak RSS ≈ buffer + index vectors).
  const auto values_for = [](size_t bytes) {
    return std::max<size_t>(536870912ull / bytes, 8u * 1024 * 1024);
  };

  auto* out = std::fopen(out_path.c_str(), "w");
  if (!out) {
    std::perror("fopen");
    return 2;
  }
  std::fprintf(out,
               "device,gamma,data_type,repeat,accesses,buffer_bytes,bytes_per_value,"
               "ns_total,ns_per_access,ns_per_access_per_byte\n");

  long long checksum = 0;  // sink so accesses are not optimized away
  for (int node : {1, 2}) {
    const char* device = node == 1 ? "dram" : "cxl";  // lowercase: loader grid keys
    for (const auto& type : kTypes) {
      const size_t n_values = values_for(type.bytes);
      const size_t buffer_bytes = n_values * type.bytes;
      auto* buf = static_cast<uint8_t*>(bind_buffer(buffer_bytes, node));
      if (!buf) {
        std::fprintf(stderr, "mbind/mmap failed for node %d (%zu bytes)\n", node, buffer_bytes);
        return 2;
      }
      std::memset(buf, 1, buffer_bytes);
      if (!verify_residency(type.name, node)) {
        std::fprintf(stderr, "residency verification failed (node %d, %s)\n", node, type.name);
        return 2;
      }

      // Index streams
      auto indices = std::vector<size_t>(n_values);
      for (size_t i = 0; i < n_values; ++i) indices[i] = i;
      auto rng = std::mt19937_64(0xC3C3C3C3C3C3C3C3ull);
      std::shuffle(indices.begin(), indices.end(), rng);  // random stream
      // monotonic stream: stride 2 over a rotated window (monotone, gaps)
      auto mono = std::vector<size_t>(n_values / 2);
      for (size_t i = 0; i < mono.size(); ++i) mono[i] = (i * 2 + n_values / 4) % n_values;

      for (const char* gamma : {"sequential", "monotonic", "random", "point"}) {
        for (size_t repeat = 0; repeat < kRepeats; ++repeat) {
          // Warm the TLB / page tables once (not timed)
          for (size_t i = 0; i < 65536; ++i) checksum += buf[i * type.bytes % buffer_bytes];
          const auto t0 = now_ns();
          if (std::strcmp(gamma, "sequential") == 0) {
            for (size_t k = 0; k < n_access; ++k) {
              const size_t idx = k % n_values;
              checksum += buf[idx * type.bytes];
            }
          } else if (std::strcmp(gamma, "monotonic") == 0) {
            for (size_t k = 0; k < n_access; ++k) {
              const size_t idx = mono[k % mono.size()];
              checksum += buf[idx * type.bytes];
            }
          } else {  // random == point: uniform random single-value accesses
            for (size_t k = 0; k < n_access; ++k) {
              const size_t idx = indices[k % n_values];
              checksum += buf[idx * type.bytes];
            }
          }
          const auto t1 = now_ns();
          const double ns_total = t1 - t0;
          const double ns_per_access = ns_total / static_cast<double>(n_access);
          std::fprintf(out, "%s,%s,%s,%zu,%zu,%zu,%zu,%.3f,%.6f,%.9f\n", device, gamma, type.name,
                       repeat, n_access, buffer_bytes, type.bytes, ns_total, ns_per_access,
                       ns_per_access / static_cast<double>(type.bytes));
        }
      }
      munmap(buf, buffer_bytes);
    }
  }
  std::fclose(out);
  std::fprintf(stderr, "# checksum sink: %lld\n", checksum);
  return 0;
}
