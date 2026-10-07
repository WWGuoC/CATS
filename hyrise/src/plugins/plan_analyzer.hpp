#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "hyrise.hpp"
#include "logical_query_plan/abstract_lqp_node.hpp"
#include "storage/segment_access_counter.hpp"
#include "types.hpp"

namespace hyrise {

// Placement target tier for a segment.
enum class Tier : uint8_t { Local, CXL, Both };

// Access type derived from LQP operator analysis (Δ1 Stage 1: Plan Analyzer).
enum class AccessType : uint8_t { Scan, PointLookup, HashBuild, HashProbe, Aggregate, Sort, Unknown };

// Per-segment placement decision produced by Δ1 stages 1-4.
// Stages 1-3 fill table_name/chunk_id/column_id/access_type/cardinality/volume/frequency/is_immutable.
// Stage 4 (PolicyMapper) fills target_tier and create_replica.
struct SegmentPlacementDecision {
  std::string table_name;
  ChunkID chunk_id{0};
  ColumnID column_id{0};
  AccessType access_type = AccessType::Unknown;
  float estimated_cardinality = 0.0f;
  uint64_t data_volume = 0;       // bytes
  uint64_t access_frequency = 0;  // from SegmentAccessCounter (profiling pass)
  bool is_immutable = false;      // chunk->is_mutable() == false
  Tier target_tier = Tier::Local;
  bool create_replica = false;    // true → M2 bandwidth aggregation
};

// Δ1 stages 1-3: Plan Analyzer → Object Resolver → Characteristic Tagger.
// Traverses optimized LQPs to extract per-segment access signals, resolves to
// concrete segment lists using ChunkPruningRule output, and tags each segment
// with a feature vector.
class PlanAnalyzer {
 public:
  // Main entry: analyze the workload and produce per-segment decisions.
  // Must be called AFTER the profiling pass (execute_item for each item),
  // so SegmentAccessCounter is populated.
  std::vector<SegmentPlacementDecision> analyze(AbstractBenchmarkItemRunner& runner, bool ablate_d1);

  // Helper: read access frequency from SegmentAccessCounter for a segment.
  static uint64_t get_access_frequency(const std::shared_ptr<AbstractSegment>& segment);

  // [IMPL-4] (plan v3 §4, E9B): cardinality-estimate perturbation factor kappa.
  // Multiplied into every estimated_cardinality read in analyze_lqps().
  // Default 1.0 = identity (zero perturbation; frozen paths bit-identical).
  void set_kappa(float kappa) { _kappa = kappa; }
  float kappa() const { return _kappa; }

  // [IMPL-5] (plan v3 §4, E10): override the directory build_job_lqps() reads
  // .sql files from. Default = third_party/join-order-benchmark (wave-1
  // behavior). Set to the benchmark's actual --query_path so plan analysis
  // follows the workload being run (synthetic mixed-workload driver).
  void set_query_dir(const std::string& dir) { _query_dir = dir; }
  const std::string& query_dir() const { return _query_dir; }

  // EDBT v4 Phase B (commit B1) ------------------------------------------------
  // Union of LQP consuming-operator names for a column, accumulated during
  // analyze_lqps() ONLY when placement_options.placement_trace is on (pure
  // observation; never read by the placement pipeline). Returns "-" when the
  // column was never classified (ablate_d1 arm / unreferenced column).
  const std::string& operator_mask(const std::string& table_name, ColumnID column_id) const;

  // EDBT v4 Phase B (commit B1): write the operator/segment reference trace
  // (schemas/operator_segment_trace.schema.json) as plan_reference_only rows —
  // one row per (query, StoredTableNode occurrence, referenced chunk, unpruned
  // column). Per-execution attribution is not measurable under clients=10
  // concurrency, so measured traffic fields stay empty and every row declares
  // attribution_level=plan_reference_only. Observation-only: not part of the
  // placement pipeline.
  void write_operator_reference_trace(const std::string& path, const std::string& run_id,
                                      const std::string& config, uint32_t rep);

 private:
  // Stage 1: Plan Analyzer — build SQL pipelines, traverse LQPs, extract
  // (table_name, column_id, access_type, cardinality) per query.
  struct ColumnAccessSignal {
    std::string table_name;
    ColumnID column_id{0};
    AccessType access_type = AccessType::Unknown;
    float estimated_cardinality = 0.0f;
    // Chunks that are PRUNED (will NOT be accessed). Empty = all chunks accessed.
    std::vector<ChunkID> pruned_chunk_ids;
  };

  std::vector<ColumnAccessSignal> analyze_lqps();

  // [IMPL-4] cardinality perturbation factor (see set_kappa); 1.0 = identity
  float _kappa = 1.0f;

  // [IMPL-5] JOB query directory (see set_query_dir); default = wave-1 path
  std::string _query_dir = "third_party/join-order-benchmark";

  // EDBT v4 Phase B (B1): per-column set of consuming-operator names, filled by
  // traverse_lqp only when placement_options.placement_trace is on. Lazily
  // joined into '|'-separated strings by operator_mask().
  mutable std::map<std::string, std::set<std::string>> _column_operator_names;
  mutable std::map<std::string, std::string> _column_operator_masks;

  // Build SQL pipelines for TPC-H queries (using deterministic parameters).
  std::vector<std::shared_ptr<AbstractLQPNode>> build_tpch_lqps();

  // Build SQL pipelines for TPC-C representative queries (one per transaction type).
  std::vector<std::shared_ptr<AbstractLQPNode>> build_tpcc_lqps();

  // Build SQL pipelines for JOB (Join Order Benchmark) queries by reading
  // .sql files from third_party/join-order-benchmark/.
  std::vector<std::shared_ptr<AbstractLQPNode>> build_job_lqps();

  // Traverse a single optimized LQP root, extracting column access signals.
  void traverse_lqp(const std::shared_ptr<AbstractLQPNode>& root, std::vector<ColumnAccessSignal>& signals);

  // Classify the access type for a StoredTableNode by walking upward to the
  // first consuming operator (Predicate/Join/Aggregate/Sort).
  // EDBT v4 Phase B (B1): optional consumer_name out-param (observation-only).
  AccessType classify_access_type(const std::shared_ptr<AbstractLQPNode>& stored_table_node,
                                  std::string* consumer_name = nullptr);

  // Stage 2: Object Resolver — expand (table, pruned_chunk_ids, column_id)
  // into concrete (table, chunk_id, column_id) segment lists.
  std::vector<SegmentPlacementDecision> resolve_segments(const std::vector<ColumnAccessSignal>& signals);

  // Stage 3: Characteristic Tagger — tag each segment with volume, frequency,
  // is_immutable.
  void tag_segments(std::vector<SegmentPlacementDecision>& decisions);
};

}  // namespace hyrise
