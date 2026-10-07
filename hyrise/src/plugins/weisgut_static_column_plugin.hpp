#pragma once

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "memory/linear_numa_memory_resource.hpp"
#include "utils/abstract_plugin.hpp"
#include "storage/chunk.hpp"
#include "storage/table.hpp"
#include "types.hpp"

namespace hyrise {

// ── EDBT v4 Phase W: Weisgut-style static whole-column placement baseline ────
//
// Spec: 07_weisgut_static_column/GLM53_EXPERIMENT_SPEC.md §5. This plugin
// re-implements the baseline for the CURRENT engine; per §3 it must be
// described as "our reimplementation of a Weisgut-style static column
// placement baseline" in the paper.
//
// Profile mode  (--column_profile_out <csv>): observe-only. No placement, no
//   daemon, no replicas. post_benchmark_hook dumps column_profile.csv with
//   column_access_count = Σ over all segments of the five typed access
//   counters (Point/Sequential/Monotonic/Random/Dictionary) — the pure sum
//   the Hyrise upstream frequency placement consumes — plus the live column
//   byte size. Nothing from the timed phase feeds back into any selection.
//
// Static mode   (--weisgut_static_column --column_profile_path <frozen.csv>):
//   reads the FROZEN profile, sorts columns by access_count desc (ties:
//   table_name asc, column_id asc), then greedy whole-column packing against
//   the exact byte budget (float32(CLI GiB) × 2^30 truncated): a column is
//   selected iff adding its full size keeps the running total ≤ budget;
//   oversized columns are skipped and the scan continues (spec §5.3.4).
//   Selected columns are fully copied to local DRAM (node1), all other
//   columns to CXL (node2) BEFORE the timed phase; the mapping is frozen:
//   no daemon, no replica machinery, no timed-phase migration. Emits
//   selected_columns.csv, placement_map_start.csv (pre) and
//   placement_map_end.csv + overhead.csv (post; end must equal start).
//
// Deliberately separate from SemanticPlacementPlugin: our_full's decision
// code is untouched by this file.
class WeisgutStaticColumnPlugin : public AbstractPlugin {
 public:
  std::string description() const final;
  void start() final;
  void stop() final;

  std::optional<PreBenchmarkHook> pre_benchmark_hook() final;
  std::optional<PostBenchmarkHook> post_benchmark_hook() final;

 private:
  struct ColumnRow {
    std::string table_name;
    ColumnID::base_type column_id{0};
    std::string column_name;
    uint64_t access_count{0};
    uint64_t column_bytes{0};
    uint64_t cumulative_bytes{0};
    bool selected{false};
    std::string skip_reason{};
  };

  bool _static_mode = false;
  bool _profile_mode = false;
  std::vector<ColumnRow> _columns;
  uint64_t _budget_bytes = 0;
  uint64_t _selected_bytes = 0;
  std::chrono::steady_clock::time_point _placement_t0;
  uint64_t _placement_time_ms = 0;
  uint64_t _placement_bytes = 0;
  uint64_t _migrated_segments = 0;
  uint64_t _mutable_segments_skipped = 0;
  std::string _profile_path;
  std::string _profile_out_path;
  std::string _map_start_path;
  std::string _map_end_path;
  std::string _selected_columns_path;
  std::string _overhead_path;
  // Raw pointers into Hyrise::get().memory_resources: the GLOBAL map owns
  // the resources (semantic-plugin pattern), so segments allocated from them
  // survive plugin teardown and are destroyed before the resources during
  // Hyrise shutdown. Plugin-member ownership caused exit-time use-after-free.
  LinearNumaMemoryResource* _local_resource = nullptr;
  LinearNumaMemoryResource* _cxl_resource = nullptr;

  // Placement bookkeeping: tier of each (table, chunk, column) AFTER the
  // one-time placement. The segment OBJECT lives on the default heap
  // (make_shared in copy_using_memory_resource) — only its DATA vector is
  // allocated from the LinearNuma buffer — so physical tier is recorded from
  // the placement action, and the runner verifies node residency in aggregate
  // via /proc/<pid>/numa_maps sampling.
  std::unordered_map<std::string, std::string> _tier_map;

  void _read_profile(const std::string& path);
  void _pack_columns();
  void _place_columns();
  void _write_selected_columns_csv() const;
  void _write_placement_map(const std::string& path) const;
  void _write_overhead_csv() const;
  void _dump_profile() const;
};

}  // namespace hyrise
