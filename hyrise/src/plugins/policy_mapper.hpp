#pragma once

#include <algorithm>
#include <cstdint>
#include <numeric>
#include <vector>

#include "plan_analyzer.hpp"

namespace hyrise {

// EDBT v4 Phase B (commit B2): the one shared deterministic strict-fit mapper.
// Used by EVERY migration policy (our_full, our_freq/--ablate_d1 a.k.a.
// our_no_plan, and — from commit B3 — cost_c3_greedy); only the per-segment
// score/benefit provider differs between policies, never the selection rule.
//
// Ordering (total, deterministic):
//   1. score descending,
//   2. table name ascending,
//   3. chunk id ascending,
//   4. column id ascending.
// Selection (strict fit): walking that order, a segment is selected for DRAM
// only when `segment_bytes <= remaining_budget`; segments that do not fit are
// skipped (later, smaller segments may still fit). Selected logical bytes can
// therefore never exceed the budget — the v3 boundary-overshoot behavior
// (capacity-accumulation prefix that admitted an oversized boundary segment)
// is replaced for all policies by this rule.
struct StrictFitResult {
  // Segment indices in comparator order; rank r ↔ order[r].
  std::vector<size_t> order;
  // selected[segment index] = 1 iff the segment is placed on DRAM this cycle.
  std::vector<char> selected;
  // remaining_budget_before[rank] = budget still remaining when that rank was
  // considered (trace/audit support; selected ⇒ bytes fit this value).
  std::vector<uint64_t> remaining_budget_before;
  uint64_t selected_bytes = 0;
  size_t selected_count = 0;
};

template <typename SegmentLike>
StrictFitResult strict_fit_map(const std::vector<SegmentLike>& segments,
                               const std::vector<double>& scores,
                               const uint64_t budget_bytes) {
  auto result = StrictFitResult{};
  result.order.resize(segments.size());
  std::iota(result.order.begin(), result.order.end(), size_t{0});
  std::sort(result.order.begin(), result.order.end(), [&](const size_t a, const size_t b) {
    if (scores[a] != scores[b]) return scores[a] > scores[b];
    const auto& seg_a = segments[a];
    const auto& seg_b = segments[b];
    if (seg_a.table_name != seg_b.table_name) return seg_a.table_name < seg_b.table_name;
    if (seg_a.chunk_id != seg_b.chunk_id) return seg_a.chunk_id < seg_b.chunk_id;
    return seg_a.column_id < seg_b.column_id;
  });

  result.selected.assign(segments.size(), 0);
  result.remaining_budget_before.resize(segments.size());
  auto remaining = budget_bytes;
  for (auto rank = size_t{0}; rank < result.order.size(); ++rank) {
    const auto idx = result.order[rank];
    const auto segment_bytes = segments[idx].data_volume;
    result.remaining_budget_before[rank] = remaining;
    if (segment_bytes <= remaining) {
      result.selected[idx] = 1;
      remaining -= segment_bytes;
      result.selected_bytes += segment_bytes;
      ++result.selected_count;
    }
  }
  return result;
}

// Δ1 Stage 4: Policy Mapper — applies decision rules to each segment,
// assigning a target tier.
//
// Decision table:
//   hot scan  → Local (frequent access, keep in fast DRAM)
//   cold scan → CXL   (rare access, move to slow tier)
//   point-lookup hot  → Local
//   point-lookup cold → CXL
//   hash-build large → CXL (large hash-build tables moved to CXL)
//   aggregate → Local (CPU-bound)
//   hash-probe hot → Local
//   immutable cold → CXL (zero consistency cost)
class PolicyMapper {
 public:
  // Apply decision rules to all segments. Modifies target_tier in-place.
  void map(std::vector<SegmentPlacementDecision>& decisions);

  // DRAM-first placement: data starts on DRAM. Demote the coldest demote_pct
  // fraction of segments to CXL (prefer immutable + low-frequency). Remaining
  // segments stay on Local (DRAM). This produces the initial placement that the
  // daemon will refine during benchmark execution.
  // dram_budget_bytes > 0 additionally clamps the Local (DRAM) keep-set to the
  // EXACT byte budget: after the demote_pct fraction split, the coldest
  // remaining Local segments are demoted until the Local byte sum fits.
  // Mandated by the Phase W repair spec (G1/G2: logical DRAM bytes must not
  // exceed the exact budget — the segment-count fraction alone overshot on
  // TPC-H's skewed segment sizes, e.g. 4.66 GB vs the 4.40 GB c05 budget).
  void map_dram_first(std::vector<SegmentPlacementDecision>& decisions, float demote_pct,
                      uint64_t dram_budget_bytes = 0);

 private:
  // Compute frequency thresholds from the decisions.
  // hot_threshold = 70th percentile, cold_threshold = 30th percentile.
  void compute_thresholds(const std::vector<SegmentPlacementDecision>& decisions,
                           uint64_t& hot_threshold, uint64_t& cold_threshold, uint64_t& large_volume_threshold);
};

}  // namespace hyrise
