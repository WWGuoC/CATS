#pragma once

#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "hyrise.hpp"
#include "plan_analyzer.hpp"
#include "policy_mapper.hpp"
#include "utils/abstract_plugin.hpp"

namespace hyrise {

// SemanticPlacementPlugin: implements Δ1 (query-plan-driven proactive placement,
// 5-stage pipeline), Δ2 (segment-granularity placement), Δ3 (replica management
// + GC), with 4 ablation switches.
//
// Two placement modes:
//   - CXL-first (default, legacy): data starts on CXL, hot segments promoted to DRAM.
//   - DRAM-first (--dram_first): data starts on DRAM, cold segments demoted to CXL,
//     then a periodic daemon refines placement during benchmark execution.
//
// pre_benchmark_hook:
//   1. Create memory resources (cxl=node2, local=node1)
//   2. Profiling pass (execute all items once — populates SegmentAccessCounter)
//   3. Δ1 stages 1-3: PlanAnalyzer.analyze() → per-segment decisions
//   4. Δ1 stage 4: PolicyMapper (DRAM-first or CXL-first) → set target_tier
//   5. Δ1 stage 5 (Action Dispatcher): execute placement
//   6. Write placement CSV
//   7. Initialize placement tracking map
//   8. Start daemon thread (if daemon_period_seconds > 0)
//
// post_benchmark_hook:
//   - Stop daemon thread
//   - Δ3 GC-1: reclaim staging replicas
//   - Write metrics to JSON report (including daemon migration stats)

// Key for identifying a single segment across tables/chunks/columns.
struct SegmentKey {
  std::string table_name;
  ChunkID chunk_id{0};
  ColumnID column_id{0};

  bool operator==(const SegmentKey& other) const {
    return table_name == other.table_name && chunk_id == other.chunk_id && column_id == other.column_id;
  }
};

struct SegmentKeyHash {
  size_t operator()(const SegmentKey& key) const {
    return std::hash<std::string>{}(key.table_name) ^
           (std::hash<ChunkID::base_type>{}(key.chunk_id) << 16) ^
           (std::hash<ColumnID::base_type>{}(key.column_id) << 8);
  }
};

class SemanticPlacementPlugin : public AbstractPlugin {
 public:
  SemanticPlacementPlugin() = default;

  std::string description() const final;
  void start() final;
  void stop() final;
  std::vector<std::pair<PluginFunctionName, PluginFunctionPointer>> provided_user_executable_functions() final;
  std::optional<PreBenchmarkHook> pre_benchmark_hook() final;
  std::optional<PostBenchmarkHook> post_benchmark_hook() final;

 private:
  // Δ1 stage 5: Action Dispatcher — execute placement decisions
  void execute_placement(std::vector<SegmentPlacementDecision>& decisions);

  // Δ2 ON: per-segment migration
  void migrate_segment(const SegmentPlacementDecision& decision);

  // Δ2 OFF: column-level migration (collect per-table, then migrate_columns)
  void migrate_columns(std::vector<SegmentPlacementDecision>& decisions);

  // Write placement decisions to CSV (same path pattern as B2)
  void write_placement_csv(const std::vector<SegmentPlacementDecision>& decisions);

  // ── Daemon (periodic migration) ──────────────────────────────────────────
  // Background thread loop: sleep(daemon_period) → scan_and_migrate()
  void daemon_loop();

  // One migration cycle: scan all segments, compute P(s) priority, promote/demote
  void scan_and_migrate();

  // Semantic priority function P(s) — higher = hotter (promote), lower = colder (demote)
  double compute_priority(AccessType access_type, uint64_t access_rate, uint64_t data_volume,
                          bool is_immutable, uint64_t max_rate, uint64_t max_volume) const;

  // Promote a single segment from CXL to Local (DRAM), respecting capacity.
  // Always saves the old CXL segment as a replica for zero-copy demote.
  bool promote_segment(const std::string& table_name, ChunkID chunk_id, ColumnID column_id);

  // Demote a single segment from Local (DRAM) to CXL
  bool demote_segment(const std::string& table_name, ChunkID chunk_id, ColumnID column_id);

  // Read total access frequency from a segment's access counter (atomic-safe)
  uint64_t get_segment_access_frequency(const std::shared_ptr<AbstractSegment>& segment) const;

  // Components
  PlanAnalyzer _plan_analyzer;
  PolicyMapper _policy_mapper;

  // Memory resources (raw pointers — owned by Hyrise::memory_resources)
  LinearNumaMemoryResource* _cxl_resource = nullptr;
  LinearNumaMemoryResource* _local_resource = nullptr;

  // Ablation flags (from placement_options)
  bool _ablate_d1 = false;
  bool _ablate_d2 = false;
  bool _ablate_d3_replica = false;  // true → disable _cxl_replicas (no zero-copy demote)

  // DRAM-first mode + daemon configuration
  bool _dram_first = false;
  uint32_t _daemon_period_seconds = 0;  // 0 = no daemon

  // [IMPL-2] (plan v3 §4): E8 cost-weighted frequency arm (formula A)
  bool _wfreq = false;

  // ── EDBT v4 Phase B (commit B3): cost_c3_greedy baseline provider ─────────
  // Third migration policy: priority = cost(CXL) − cost(DRAM) from the
  // calibration table and the RAW four-pattern segment access counters
  // (sequential/monotonic/random/point; no EMA — documented). Dictionary
  // accesses are counted and reported but excluded from the cost model
  // (dictionary_handling=excluded_reported). Feeds the shared strict-fit
  // mapper of commit B2 unchanged.
  bool _c3_greedy = false;
  // calibration[device][gamma][data_type] = ns_per_access_per_byte
  std::map<std::string, std::map<std::string, std::map<std::string, double>>> _c3_calibration;
  std::string _c3_calibration_sha256;
  std::string _c3_version;  // repo HEAD at plugin build time (version.hpp)

  // Parse the calibration CSV, populate _c3_calibration, compute the file's
  // SHA-256, and verify the full device × gamma × data_type grid is present.
  // Returns false (with a logged reason) on any malformed input.
  bool load_c3_calibration(const std::string& path);

  // Daemon thread infrastructure
  std::thread _daemon_thread;
  std::atomic<bool> _daemon_running{false};
  std::condition_variable _daemon_cv;
  std::mutex _daemon_mutex;

  // Placement tracking: (table, chunk, column) → current tier
  std::unordered_map<SegmentKey, Tier, SegmentKeyHash> _placement_map;

  // Last access frequency snapshot per segment (for computing delta in daemon)
  std::unordered_map<SegmentKey, uint64_t, SegmentKeyHash> _last_access;

  // EMA smoothing for stable priority computation
  static constexpr double EMA_ALPHA = 0.3;           // EMA smoothing factor for access frequency

  // EMA of absolute access frequency for stable priority computation
  std::unordered_map<SegmentKey, double, SegmentKeyHash> _ema_freq;

  // AccessType per segment (from LQP analysis, persisted for daemon use)
  std::unordered_map<SegmentKey, AccessType, SegmentKeyHash> _access_type_map;

  // CXL上的段副本: 所有DRAM中的热段均在CXL中维护一个副本
  // 映射关系: SegmentKey → CXL上的段shared_ptr
  // - promote (CXL→DRAM): 实际数据拷贝, 保留CXL旧段作为副本
  // - demote (DRAM→CXL): 零拷贝, 直接切换chunk引用到CXL副本, 释放DRAM段
  // - GC (每周期): 回收不属于热段(DRAM)的CXL副本, 避免容量堆积
  std::unordered_map<SegmentKey, std::shared_ptr<AbstractSegment>, SegmentKeyHash> _cxl_replicas;

  // Daemon metrics
  size_t _daemon_promote_count = 0;
  size_t _daemon_demote_count = 0;
  size_t _daemon_cycles = 0;
  size_t _daemon_gc_count = 0;  // CXL replicas reclaimed by per-cycle GC

  // Migration cost metrics — daemon migrations (promote/demote copy operations)
  uint64_t _daemon_migration_time_ns = 0;    // wall-clock time of daemon copy migrations
  uint64_t _daemon_bytes_migrated = 0;       // bytes moved by daemon copy migrations
  size_t _daemon_copy_count = 0;             // number of actual data-copy migrations
  size_t _daemon_zero_copy_count = 0;        // number of zero-copy demotes (replica swap, no copy)

  // ── P0 instrumentation (plan v2 §4): per-cycle daemon metrics CSV ──────────
  // Every daemon cycle appends one row to <json_path>_daemon_metrics.csv:
  //   unix_ts,cycle_id,hit_rate_pct,promotes,demotes,bytes_promoted,bytes_demoted,
  //   daemon_cpu_ms,replica_bytes
  std::string _daemon_metrics_path;
  uint64_t _daemon_bytes_promoted = 0;       // cumulative bytes promoted (CXL→DRAM copies)
  uint64_t _daemon_bytes_demoted = 0;        // cumulative bytes demoted (DRAM→CXL copies)

  // Per-cycle metrics row emitter (called at end of every scan_and_migrate cycle)
  void append_daemon_metrics_row(uint64_t access_delta_local, uint64_t access_delta_cxl,
                                 size_t snap_promotes, size_t snap_demotes,
                                 uint64_t snap_bytes_promoted, uint64_t snap_bytes_demoted,
                                 timespec cpu_t0, time_t wall_start);

  // Migration cost metrics — initial placement (pre_benchmark_hook)
  uint64_t _init_migration_time_ns = 0;      // wall-clock time of initial placement copies
  uint64_t _init_bytes_migrated = 0;         // bytes moved in initial placement
  size_t _init_copy_count = 0;               // number of initial placement copies

  // ── EDBT v4 Phase B (commit B1): observation-only trace, default OFF ───────
  // With options.placement_trace == false every member below is inert. When
  // on, the daemon appends one row per considered segment per cycle to
  // _decision_trace_path (schemas/decision_trace.schema.json) and the
  // pre_benchmark_hook writes the plan_reference_only operator/segment
  // reference trace once (no per-execution attribution under clients=10).
  bool _trace_on = false;
  std::string _trace_run_id;                 // EDBT_V4_RUN_ID env or JSON stem
  std::string _trace_config;                 // our_full | our_freq | cost_c3_greedy
  uint32_t _trace_rep = 1;                   // EDBT_V4_REP env or 1
  std::string _decision_trace_path;
  std::string _operator_trace_path;
  // κ-corrected plan cardinality per segment (decisions vector; 0 = no plan
  // reference, e.g. ablate_d1 arm or never-referenced segment)
  std::unordered_map<SegmentKey, float, SegmentKeyHash> _trace_cardinality;

  // Set up trace paths/run identity, write CSV headers; called from the
  // pre_benchmark_hook when _trace_on. Also emits the [EDBT-TRACE] log line.
  void init_trace(const std::string& json_path);

  // ── EDBT v4 Phase B2: single-client measured diagnostic pass ──────────────
  // Only when _trace_on && _trace_measured (JOB CLI gates --placement_trace +
  // --clients 1). Runs after the initial placement (so tier_at_execution is
  // the static _placement_map) and BEFORE the daemon thread starts: executes
  // each workload query once, sequentially, snapshotting every segment's five
  // pattern counters before/after. Emits:
  //   <json>_measured_segment_access.csv — per (query, segment) counter deltas
  //       (attribution_level=measured_per_execution; exact because sequential)
  //   appended measured_per_execution rows in <json>_operator_segment_trace.csv
  //       per PQP TableScan over a stored table: rows/wall from the executed
  //       PQP performance data. Emitted ONLY when exactly one scan of that
  //       query references the segment's (table, column) — the pack validator
  //       contract requires actual_reads on every measured row, and reads are
  //       attributable to one scan only under that uniqueness rule (operator
  //       association is never fabricated).
  //   <json>_operator_wall_rows.csv — per-scan duration/rows for EVERY scan
  //       (attribution-free diagnostic: wall_ns/rows_in/rows_out are reliable
  //       per execution; unique_scan column documents the rule above).
  bool _trace_measured = false;
  std::string _measured_segment_access_path;
  void run_measured_diagnostic_pass();

  // warmup | timed_first_half | timed_second_half (from the runner's anchors)
  std::string trace_phase_label() const;

  // Metrics for post_benchmark_hook JSON report
  size_t _total_decisions = 0;
  size_t _local_count = 0;
  size_t _cxl_count = 0;
  size_t _replica_count = 0;
};

}  // namespace hyrise
