#include "semantic_placement_plugin.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <numeric>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

#include "../benchmarklib/abstract_benchmark_item_runner.hpp"
#include "expression/abstract_expression.hpp"
#include "expression/pqp_column_expression.hpp"
#include "hyrise.hpp"
#include "memory/linear_numa_memory_resource.hpp"
#include "operators/abstract_operator.hpp"
#include "operators/get_table.hpp"
#include "operators/table_scan.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "storage/chunk.hpp"
#include "storage/segment_access_counter.hpp"
#include "storage/table.hpp"
#include "utils/assert.hpp"
#include "version.hpp"

namespace hyrise {

// [IMPL-3] (plan v3 §4, human-approved 2026-08-31, D28): macro-overridable P(s)
// weights for the E9A perturbation arms. Defaults are the frozen v2 values; a
// build-time define (CMake -DHYRISE_PLACEMENT_W_ATYPE=0.3 etc.) overrides a single
// weight without touching any other code path. Un-normalized by design: P(s) is a
// ranking, weights need not sum to 1. The startup log line below prints the
// effective vector so every run.log self-documents which variant it used.
#ifndef HYRISE_PLACEMENT_W_ATYPE
#define HYRISE_PLACEMENT_W_ATYPE 0.50
#endif
#ifndef HYRISE_PLACEMENT_W_FREQ
#define HYRISE_PLACEMENT_W_FREQ 0.25
#endif
#ifndef HYRISE_PLACEMENT_W_VOLUME
#define HYRISE_PLACEMENT_W_VOLUME 0.15
#endif
#ifndef HYRISE_PLACEMENT_W_IMMUTABLE
#define HYRISE_PLACEMENT_W_IMMUTABLE 0.10
#endif
constexpr double kPlacementWAtype = HYRISE_PLACEMENT_W_ATYPE;
constexpr double kPlacementWFreq = HYRISE_PLACEMENT_W_FREQ;
constexpr double kPlacementWVolume = HYRISE_PLACEMENT_W_VOLUME;
constexpr double kPlacementWImmutable = HYRISE_PLACEMENT_W_IMMUTABLE;

std::string SemanticPlacementPlugin::description() const {
  return "SemanticPlacementPlugin";
}

void SemanticPlacementPlugin::start() {}
void SemanticPlacementPlugin::stop() {}

std::vector<std::pair<PluginFunctionName, PluginFunctionPointer>>
SemanticPlacementPlugin::provided_user_executable_functions() {
  return {};
}

// ─────────────────────────────────────────────────────────────────────────────
// pre_benchmark_hook: the main entry point for Δ1+Δ2+Δ3
// ─────────────────────────────────────────────────────────────────────────────
std::optional<PreBenchmarkHook> SemanticPlacementPlugin::pre_benchmark_hook() {
  return [this](auto& benchmark_item_runner) {
    const auto& options = Hyrise::get().placement_options;

    // Set ablation flags
    _ablate_d1 = options.ablate_d1;
    _ablate_d2 = options.ablate_d2;
    _ablate_d3_replica = options.ablate_d3_replica;

    // Set DRAM-first + daemon config
    _dram_first = options.dram_first;
    _daemon_period_seconds = options.daemon_period_seconds;
    _wfreq = options.placement_wfreq;
    // [IMPL-4] E9B: kappa flows CLI -> PlacementOptions -> PlanAnalyzer (identity unless set)
    _plan_analyzer.set_kappa(options.placement_kappa);
    // [IMPL-5] E10: plan analysis follows the benchmark's actual --query_path
    // (empty = default third_party path, wave-1 behavior unchanged)
    if (!options.placement_query_path.empty()) {
      _plan_analyzer.set_query_dir(options.placement_query_path);
    }

    // [EDBT v4 B3] cost_c3_greedy: load + verify the calibration table and
    // print the required BASELINE startup line before any placement work.
    // version = repo HEAD at plugin build time; calibration_sha256 covers the
    // exact calibration bytes; the counter set is the segment access counter's
    // four pattern classes (raw cumulative values, no EMA).
    _c3_greedy = options.cost_c3_greedy;
    if (_c3_greedy) {
      _c3_version = GIT_HEAD_SHA1;
      if (!load_c3_calibration(options.c3_calibration_path)) {
        Assert(false, "cost_c3_greedy: calibration table missing/invalid (see log above)");
      }
      std::cout << "BASELINE cost_c3_greedy version=" << _c3_version
                << " calibration_sha256=" << _c3_calibration_sha256
                << " counter_set=sequential,monotonic,random,point\n";
    }

    std::cout << "=== SemanticPlacementPlugin pre_benchmark_hook ===\n";
    if (_dram_first) {
      std::cout << "- Mode: DRAM-first (data starts on DRAM, demote cold to CXL)\n";
    } else {
      std::cout << "- Mode: CXL-first (data starts on CXL, promote hot to DRAM)\n";
    }
    if (_daemon_period_seconds > 0) {
      std::cout << "- Daemon: ON (period=" << _daemon_period_seconds << "s)\n";
    } else {
      std::cout << "- Daemon: OFF\n";
    }
    // [IMPL-1] evidence line: run must be greppable for the freeze intent at startup.
    if (Hyrise::get().placement_options.placement_freeze_at_timed_start) {
      std::cout << "- Placement freeze: AT TIMED START (warmup places, timed is static)\n";
    }
    // [IMPL-3] evidence line: effective P(s) weight vector of this build (E9A arms).
    std::cout << "- [IMPL-3] P(s) weights: atype=" << kPlacementWAtype
              << " freq=" << kPlacementWFreq
              << " volume=" << kPlacementWVolume
              << " immutable=" << kPlacementWImmutable << "\n";
    // [IMPL-2] evidence line: E8 cost-weighted frequency arm (formula A, human-specified
    // 2026-08-31). freq_w(s) = Σ_a cost(atype(a)) = cost(atype(s)) × access_rate(s),
    // cost = semantic score table (1.0/0.8/0.7/0.6/0.5/0.4/Unknown 0.5), normalized by
    // this round's max freq_w; P(s) keeps the frequency-arm shape (atype=Unknown=0.5).
    if (_wfreq) {
      std::cout << "- [IMPL-2] signal: wfreq (freq_w = cost(atype) x access_rate; "
                   "cost=sem score table 1.0/0.8/0.7/0.6/0.5/0.4/0.5; norm=round max; "
                   "P(s) atype term neutralized as in the frequency arm)\n";
    }
    // [IMPL-4] evidence line: E9B cardinality perturbation. Printed on every
    // run (value itself distinguishes arms); kappa=1.0 is the identity arm.
    std::cout << "- [IMPL-4] cardinality kappa: " << _plan_analyzer.kappa() << "\n";

    // 1. Create memory resources (local=DRAM, cxl=CXL)
    // Buffer size: use explicit capacity_gb if set (>0), else fall back to scale_factor × SIZE_GIB.
    // This allows capacity-constrained experiments (e.g. DRAM=20% of data) to force cold
    // segments onto CXL even when the DRAM node has ample physical memory.
    const auto local_buf = options.dram_capacity_gb > 0.0f
                               ? static_cast<uint64_t>(options.dram_capacity_gb * SIZE_GIB)
                               : static_cast<uint64_t>(options.scale_factor * SIZE_GIB * 1.0);
    const auto cxl_buf = options.cxl_capacity_gb > 0.0f
                             ? static_cast<uint64_t>(options.cxl_capacity_gb * SIZE_GIB)
                             : static_cast<uint64_t>(options.scale_factor * SIZE_GIB * 1.0);
    std::cout << "- DRAM buffer: " << (local_buf / (1024.0 * 1024 * 1024)) << " GB"
              << ", CXL buffer: " << (cxl_buf / (1024.0 * 1024 * 1024)) << " GB\n";
    if (!options.rmem_node_ids.empty()) {
      auto memory_resource = std::make_unique<LinearNumaMemoryResource>(
          cxl_buf, options.rmem_node_ids, options.rmem_weights);
      Hyrise::get().memory_resources.emplace("cxl", std::move(memory_resource));
    }
    if (!options.lmem_node_ids.empty()) {
      auto memory_resource = std::make_unique<LinearNumaMemoryResource>(
          local_buf, options.lmem_node_ids);
      Hyrise::get().memory_resources.emplace("local", std::move(memory_resource));
    }

    auto& resources = Hyrise::get().memory_resources;
    if (!resources.contains("cxl")) {
      return;
    }

    _cxl_resource = dynamic_cast<LinearNumaMemoryResource*>(resources["cxl"].get());
    _local_resource = resources.contains("local")
                          ? dynamic_cast<LinearNumaMemoryResource*>(resources["local"].get())
                          : nullptr;
    Assert(_cxl_resource, "Failed to cast CXL resource to LinearNumaMemoryResource");
    Assert(_local_resource, "Failed to cast local resource to LinearNumaMemoryResource");

    // 2. Profiling pass (populates SegmentAccessCounter — same as B2)
    for (const auto item_id : benchmark_item_runner.items()) {
      benchmark_item_runner.execute_item(item_id);
    }

    // 3. Δ1 stages 1-3: Plan Analyzer → Object Resolver → Characteristic Tagger
    auto decisions = _plan_analyzer.analyze(benchmark_item_runner, _ablate_d1);

    // 4. Δ1 stage 4: Policy Mapper — choose direction based on mode
    if (_dram_first) {
      _policy_mapper.map_dram_first(decisions, options.demote_pct, local_buf);
    } else {
      _policy_mapper.map(decisions);
    }

    // 5. Δ1 stage 5: Action Dispatcher
    execute_placement(decisions);

    // 6. Write placement CSV
    write_placement_csv(decisions);

    // Track metrics for post_benchmark_hook
    _total_decisions = decisions.size();
    _local_count = 0;
    _cxl_count = 0;
    _replica_count = 0;
    for (const auto& d : decisions) {
      switch (d.target_tier) {
        case Tier::Local: ++_local_count; break;
        case Tier::CXL: ++_cxl_count; break;
        default: break;
      }
    }

    // 7. Initialize placement tracking map for daemon
    for (const auto& d : decisions) {
      SegmentKey key{d.table_name, d.chunk_id, d.column_id};
      _placement_map[key] = d.target_tier;
      _last_access[key] = d.access_frequency;
      _access_type_map[key] = d.access_type;                        // persist semantic type for daemon
      _ema_freq[key] = static_cast<double>(d.access_frequency);     // warm-start EMA with profiling freq
    }

    std::cout << "- Placement complete: " << _total_decisions << " decisions ("
              << _local_count << " Local, " << _cxl_count << " CXL)\n";
    if (!_ablate_d3_replica) {
      std::cout << "- Replica (zero-copy demote): ENABLED\n";
    } else {
      std::cout << "- Replica (zero-copy demote): DISABLED (ablation)\n";
    }

    // EDBT v4 Phase B (B1): observation-only trace setup (default off; inert
    // for every decision/migration path when the flag is absent).
    _trace_on = options.placement_trace;
    if (_trace_on) {
      init_trace(options.json_path);
      for (const auto& d : decisions) {
        _trace_cardinality[{d.table_name, d.chunk_id, d.column_id}] = d.estimated_cardinality;
      }
      _plan_analyzer.write_operator_reference_trace(_operator_trace_path, _trace_run_id, _trace_config,
                                                    _trace_rep);
      // [EDBT v4 B2] single-client measured diagnostic pass: after the initial
      // placement (static tiers), strictly before the daemon starts. The JOB
      // CLI gates this to --clients 1; inert in every clients=10 run.
      _trace_measured = options.placement_trace_measured;
      if (_trace_measured) {
        run_measured_diagnostic_pass();
      }
    }

    // 8. Start daemon thread (if enabled)
    if (_daemon_period_seconds > 0) {
      // P0-1: initialize per-cycle daemon metrics CSV (same dir as benchmark JSON)
      constexpr auto dot_json_length = 5u;  // length of ".json"
      _daemon_metrics_path =
          options.json_path.substr(0, options.json_path.size() - dot_json_length) + "_daemon_metrics.csv";
      {
        std::ofstream ofs(_daemon_metrics_path);
        ofs << "unix_ts,cycle_id,hit_rate_pct,promotes,demotes,bytes_promoted,bytes_demoted,"
               "daemon_cpu_ms,replica_bytes\n";
      }
      _daemon_running = true;
      _daemon_thread = std::thread(&SemanticPlacementPlugin::daemon_loop, this);
      std::cout << "- Daemon thread started (period=" << _daemon_period_seconds << "s"
                << ", metrics csv=" << _daemon_metrics_path << ")\n";
    }

    std::cout << "=== SemanticPlacementPlugin pre_benchmark_hook done ===\n";
  };
}

// ─────────────────────────────────────────────────────────────────────────────
// Δ1 Stage 5: Action Dispatcher — execute placement decisions
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::execute_placement(std::vector<SegmentPlacementDecision>& decisions) {
  if (_ablate_d2) {
    migrate_columns(decisions);
    return;
  }
  // Sort so that Local-bound segments (hot) are migrated first, ensuring they get
  // the limited DRAM buffer budget before colder segments. Within Local, sort by
  // descending access_frequency (hottest first). Non-Local segments keep their
  // relative order. This matters when DRAM capacity is constrained: the bump
  // allocator fills linearly, so the first Local segments to be copied win the
  // DRAM budget.
  std::stable_sort(decisions.begin(), decisions.end(),
    [](const SegmentPlacementDecision& a, const SegmentPlacementDecision& b) {
      const auto a_local = (a.target_tier == Tier::Local);
      const auto b_local = (b.target_tier == Tier::Local);
      if (a_local != b_local) return a_local;  // Local before non-Local
      if (a_local) return a.access_frequency > b.access_frequency;  // hottest Local first
      return false;  // preserve relative order for non-Local
    });
  for (const auto& decision : decisions) {
    migrate_segment(decision);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Δ2 ON: per-segment migration via Chunk::replace_segment
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::migrate_segment(const SegmentPlacementDecision& decision) {
  auto& storage_manager = Hyrise::get().storage_manager;
  if (!storage_manager.has_table(decision.table_name)) return;
  auto table = storage_manager.get_table(decision.table_name);
  auto chunk = table->get_chunk(decision.chunk_id);
  if (!chunk) return;
  // Skip mutable chunks: copy_using_memory_resource does not preserve the
  // pre-allocated capacity of ValueSegments, so INSERT into migrated mutable
  // chunks would fail. Only immutable (full) chunks are safe to migrate.
  if (chunk->is_mutable()) return;
  auto segment = chunk->get_segment(decision.column_id);
  if (!segment) return;

  SegmentKey key{decision.table_name, decision.chunk_id, decision.column_id};

  if (decision.target_tier == Tier::Local) {
    auto seg_size = segment->memory_usage(MemoryUsageCalculationMode::Full);
    std::shared_ptr<AbstractSegment> new_segment;
    try {
      auto t0 = std::chrono::steady_clock::now();
      new_segment = segment->copy_using_memory_resource(_local_resource);
      auto t1 = std::chrono::steady_clock::now();
      _init_migration_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      _init_bytes_migrated += seg_size;
      ++_init_copy_count;
    } catch (...) {
      // DRAM buffer full — leave segment on its current tier (CXL or default)
      return;
    }
    if (!new_segment) return;
    // 如果旧段在CXL上, 保留CXL副本 (CXL→DRAM时保留CXL副本, 用于零拷贝demote)
    if (!_ablate_d3_replica && _cxl_resource && _cxl_resource->address_in_range(segment.get())) {
      _cxl_replicas[key] = segment;
    }
    chunk->replace_segment(decision.column_id, new_segment);
    _placement_map[key] = Tier::Local;
  } else if (decision.target_tier == Tier::CXL) {
    // DRAM→CXL: 不产生副本, 不保留DRAM旧段
    auto seg_size = segment->memory_usage(MemoryUsageCalculationMode::Full);
    std::shared_ptr<AbstractSegment> new_segment;
    try {
      auto t0 = std::chrono::steady_clock::now();
      new_segment = segment->copy_using_memory_resource(_cxl_resource);
      auto t1 = std::chrono::steady_clock::now();
      _init_migration_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
      _init_bytes_migrated += seg_size;
      ++_init_copy_count;
    } catch (...) {
      // CXL buffer full — leave segment on its current tier
      return;
    }
    if (!new_segment) return;
    chunk->replace_segment(decision.column_id, new_segment);
    _placement_map[key] = Tier::CXL;
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Δ2 OFF: column-level placement decisions (ablation)
// Aggregate per-segment decisions to per-column majority, then migrate each
// segment with the column-level decision. This tests whether segment-granularity
// decisions (Δ2) outperform column-granularity.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::migrate_columns(std::vector<SegmentPlacementDecision>& decisions) {
  // Step 1: Aggregate per-column vote — key = "table:column_id"
  auto col_votes = std::unordered_map<std::string, std::pair<int, int>>{};
  for (const auto& d : decisions) {
    auto ss = std::stringstream{};
    ss << d.table_name << ":" << d.column_id;
    if (d.target_tier == Tier::Local) {
      ++col_votes[ss.str()].first;   // local votes
    } else {
      ++col_votes[ss.str()].second;  // cxl/both votes
    }
  }

  // Step 2: Override each segment's tier to its column's majority decision
  for (auto& d : decisions) {
    auto ss = std::stringstream{};
    ss << d.table_name << ":" << d.column_id;
    const auto& votes = col_votes[ss.str()];
    d.target_tier = (votes.first >= votes.second) ? Tier::Local : Tier::CXL;
    d.create_replica = false;  // no replicas in column-level mode
  }

  // Step 3: Migrate each segment with the column-level decision
  for (const auto& d : decisions) {
    migrate_segment(d);
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Write placement decisions to CSV
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::write_placement_csv(const std::vector<SegmentPlacementDecision>& decisions) {
  const auto& options = Hyrise::get().placement_options;
  auto csv_path = options.json_path;
  constexpr auto dot_json_length = 5u;  // length of ".json"
  csv_path = csv_path.substr(0, csv_path.size() - dot_json_length) + "_semantic_placement.csv";

  std::ofstream ofs(csv_path);
  ofs << "table_name,chunk_id,column_id,access_type,access_frequency,target_tier,"
         "create_replica,is_immutable,data_volume\n";
  for (const auto& d : decisions) {
    ofs << d.table_name << "," << d.chunk_id << "," << d.column_id << ","
        << static_cast<int>(d.access_type) << "," << d.access_frequency << ","
        << static_cast<int>(d.target_tier) << "," << (d.create_replica ? 1 : 0) << ","
        << (d.is_immutable ? 1 : 0) << "," << d.data_volume << "\n";
  }
  std::cout << "- Wrote placement CSV to " << csv_path << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Daemon: periodic migration thread
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::daemon_loop() {
  while (_daemon_running) {
    {
      std::unique_lock<std::mutex> lock(_daemon_mutex);
      _daemon_cv.wait_for(lock, std::chrono::seconds(_daemon_period_seconds),
                          [this] { return !_daemon_running; });
    }
    if (!_daemon_running) break;
    try {
      scan_and_migrate();
    } catch (const std::exception& e) {
      std::cerr << "- [Daemon] Error in scan_and_migrate: " << e.what() << "\n";
    }
  }
  std::cout << "- [Daemon] Thread exiting (cycles=" << _daemon_cycles
            << ", promotes=" << _daemon_promote_count
            << ", demotes=" << _daemon_demote_count << ")\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// Daemon: one migration cycle
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::scan_and_migrate() {
  ++_daemon_cycles;

  // P0-1: per-cycle snapshots for delta metrics + daemon-thread CPU time
  const auto snap_promotes = _daemon_promote_count;
  const auto snap_demotes = _daemon_demote_count;
  const auto snap_bytes_promoted = _daemon_bytes_promoted;
  const auto snap_bytes_demoted = _daemon_bytes_demoted;
  auto cpu_t0 = timespec{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_t0);
  // [IMPL-1] fixup2 (D36): row timestamp = cycle START wall clock. A cycle whose scan
  // passed the freeze check before the freeze instant, but whose migrations finish after
  // it, must still be attributed to the pre-freeze era (its decisions were pre-freeze);
  // this restores the E6 freeze-gate invariant mechanically (rows at/after the freeze
  // anchor always come from cycles that started at/after it and took the frozen branch).
  const auto cycle_wall_start = time(nullptr);
  auto access_delta_local = uint64_t{0};  // accesses this period served from DRAM
  auto access_delta_cxl = uint64_t{0};    // accesses this period served from CXL

  auto& storage_manager = Hyrise::get().storage_manager;

  // Collect per-segment info: access rate, volume, immutability, current tier
  struct SegmentInfo {
    std::string table_name;
    ChunkID chunk_id;
    ColumnID column_id;
    uint64_t access_rate;
    uint64_t data_volume;
    bool is_immutable;
    Tier current_tier;
    AccessType access_type;
    // [EDBT v4 B3] cost_c3_greedy inputs (raw cumulative counters, no EMA):
    // the four pattern classes + the separately reported Dictionary count.
    uint64_t c3_seq = 0;
    uint64_t c3_monotonic = 0;
    uint64_t c3_random = 0;
    uint64_t c3_point = 0;
    uint64_t c3_dictionary = 0;
    uint64_t c3_bytes_per_value = 0;  // from column data type (calibration grid key)
    double c3_cost_dram = 0.0;        // Σ_γ calib(dram,γ,dtype)·count_γ·bpv
    double c3_cost_cxl = 0.0;         // Σ_γ calib(cxl,γ,dtype)·count_γ·bpv
  };
  auto segments = std::vector<SegmentInfo>{};

  uint64_t max_rate = 0;
  uint64_t max_volume = 1;  // avoid division by zero
  uint64_t max_c3_pattern_total = 0;  // [B3] max raw Σ(four patterns) for norm_freq

  for (auto& [table_name, table] : storage_manager.tables()) {
    const auto chunk_count = table->chunk_count();
    const auto column_count = table->column_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      auto chunk = table->get_chunk(chunk_id);
      if (!chunk || chunk->is_mutable()) continue;
      for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
        auto segment = chunk->get_segment(col_id);
        if (!segment) continue;

        SegmentKey key{table_name, chunk_id, col_id};
        auto current_freq = get_segment_access_frequency(segment);
        // EMA of absolute access frequency (not delta) — stable hot/cold differentiation
        auto ema_it = _ema_freq.find(key);
        if (ema_it == _ema_freq.end()) {
          _ema_freq[key] = static_cast<double>(current_freq);
        } else {
          ema_it->second = EMA_ALPHA * current_freq + (1.0 - EMA_ALPHA) * ema_it->second;
        }
        auto access_rate = static_cast<uint64_t>(_ema_freq[key]);
        // P0-1: access delta vs previous cycle snapshot (hit-rate numerator/denominator).
        // _last_access is initialized from the profiling pass in pre_benchmark_hook, so
        // cycle 1's delta covers benchmark accesses of the first period.
        const auto prev_it = _last_access.find(key);
        const auto prev_freq = prev_it == _last_access.end() ? current_freq : prev_it->second;
        const auto access_delta = current_freq >= prev_freq ? current_freq - prev_freq : 0;
        _last_access[key] = current_freq;

        Tier tier = _placement_map.count(key) ? _placement_map[key] : Tier::Local;
        if (tier == Tier::Local) {
          access_delta_local += access_delta;
        } else {
          access_delta_cxl += access_delta;
        }

        // Real memory usage (accounts for encoding, data types)
        auto volume = segment->memory_usage(MemoryUsageCalculationMode::Full);

        // Use AccessType from LQP analysis (persisted in pre_benchmark_hook)
        AccessType atype = _access_type_map.count(key) ? _access_type_map[key] : AccessType::Unknown;

        if (access_rate > max_rate) max_rate = access_rate;
        if (volume > max_volume) max_volume = volume;

        auto c3_cost = std::pair<double, double>{0.0, 0.0};  // {dram, cxl}
        auto c3_counts = std::array<uint64_t, 5>{};          // seq,mon,rand,point,dict
        auto c3_bytes_per_value = uint64_t{0};
        // [EDBT v4 B2] typed-counter observation for ALL policies: the five raw
        // cumulative pattern counters (sequential, monotonic, random, point,
        // dictionary) plus the calibration dtype key / bytes-per-value are read
        // unconditionally each cycle and emitted on every trace row. The cost
        // aggregation stays exclusive to cost_c3_greedy (its calibration table
        // is only loaded in that arm); all other arms carry the counts as
        // observation-only (dictionary_handling=observed_not_scored, costs
        // empty in the trace) — they never feed those arms' priorities.
        using SAC = SegmentAccessCounter::AccessType;
        const auto& counter = segment->access_counter;
        c3_counts = {counter[SAC::Sequential].load(std::memory_order_relaxed),
                     counter[SAC::Monotonic].load(std::memory_order_relaxed),
                     counter[SAC::Random].load(std::memory_order_relaxed),
                     counter[SAC::Point].load(std::memory_order_relaxed),
                     counter[SAC::Dictionary].load(std::memory_order_relaxed)};
        // Calibration dtype key from the column's data type (trace uses the
        // same mapping). Null columns have no calibration entry; treat them
        // as 8-byte int64 (documented; JOB has none).
        const auto dtype = table->column_data_type(col_id);
        const auto* calib_type = "int64";
        switch (dtype) {
          case DataType::Int:    calib_type = "int32"; c3_bytes_per_value = 4; break;
          case DataType::Long:   calib_type = "int64"; c3_bytes_per_value = 8; break;
          case DataType::Float:  calib_type = "float32"; c3_bytes_per_value = 4; break;
          case DataType::Double: calib_type = "float64"; c3_bytes_per_value = 8; break;
          case DataType::String: calib_type = "string16"; c3_bytes_per_value = 16; break;
          case DataType::Null:   calib_type = "int64"; c3_bytes_per_value = 8; break;
        }
        if (_c3_greedy) {
          // benefit(s) = Σ_γ calib(dev,γ,dtype)·count_γ·bpv over the four C3
          // pattern classes; Dictionary counted but excluded from the cost.
          constexpr auto gammas = std::array<const char*, 4>{"sequential", "monotonic", "random", "point"};
          for (auto g = size_t{0}; g < 4; ++g) {
            const auto accesses = static_cast<double>(c3_counts[g]);
            c3_cost.first += _c3_calibration.at("dram").at(gammas[g]).at(calib_type) *
                             accesses * static_cast<double>(c3_bytes_per_value);
            c3_cost.second += _c3_calibration.at("cxl").at(gammas[g]).at(calib_type) *
                             accesses * static_cast<double>(c3_bytes_per_value);
          }
          const auto pattern_total = c3_counts[0] + c3_counts[1] + c3_counts[2] + c3_counts[3];
          if (pattern_total > max_c3_pattern_total) max_c3_pattern_total = pattern_total;
        }

        segments.push_back({table_name, chunk_id, col_id, access_rate, volume,
                           !chunk->is_mutable(), tier, atype,
                           c3_counts[0], c3_counts[1], c3_counts[2], c3_counts[3], c3_counts[4],
                           c3_bytes_per_value, c3_cost.first, c3_cost.second});
      }
    }
  }

  if (segments.empty()) {
    append_daemon_metrics_row(access_delta_local, access_delta_cxl, snap_promotes, snap_demotes,
                               snap_bytes_promoted, snap_bytes_demoted, cpu_t0, cycle_wall_start);
    return;
  }

  // [IMPL-1] (plan v3 §4): frozen placement → pure-measurement cycle. The scan above
  // (access deltas, EMA, hit_rate inputs) still runs, but no promote/demote/GC is
  // issued. Checking here — after the scan, before any migration — guarantees the E6
  // freeze gate: every daemon row appended after PLACEMENT_FROZEN has zero migration
  // columns, even for a cycle that straddles the freeze instant.
  if (Hyrise::get().placement_frozen.load(std::memory_order_relaxed)) {
    append_daemon_metrics_row(access_delta_local, access_delta_cxl, snap_promotes, snap_demotes,
                              snap_bytes_promoted, snap_bytes_demoted, cpu_t0, cycle_wall_start);
    return;
  }

  // Compute P(s) priority for each segment and sort descending
  auto priorities = std::vector<double>{};
  priorities.reserve(segments.size());
  // Hoisted (EDBT v4 B1) so the observation-only trace emitter below can read
  // the frequency arm's effective inputs; behavior identical.
  auto freq_w = std::vector<uint64_t>{};
  auto max_freq_w = uint64_t{0};
  if (_c3_greedy) {
    // [EDBT v4 B3] cost_c3_greedy provider: benefit(s) = cost(s,CXL) −
    // cost(s,DRAM), where cost(s,dev) = Σ_γ calibration(dev,γ,dtype(s)) ×
    // count_γ(s) × bytes_per_value(s) over the raw cumulative four-pattern
    // counters (computed during enumeration; Dictionary excluded/reported).
    // Feeds the SAME shared strict-fit mapper — only the score differs.
    for (size_t i = 0; i < segments.size(); ++i) {
      priorities.push_back(segments[i].c3_cost_cxl - segments[i].c3_cost_dram);
    }
  } else if (_wfreq) {
    // [IMPL-2] (plan v3 §4, formula A): cost-weighted frequency arm. freq_w(s) =
    // Σ_a cost(atype(a)); with the per-segment tracked type this equals
    // cost(atype(s)) × access_rate(s). P(s) is evaluated through the unchanged
    // compute_priority with atype=Unknown (constant 0.5 baseline, exactly like the
    // frequency arm's neutralization) and the frequency input replaced by freq_w
    // normalized by this round's max. Semantic/frequency arm paths untouched.
    freq_w = std::vector<uint64_t>(segments.size());
    for (size_t i = 0; i < segments.size(); ++i) {
      // cost table MUST stay identical to compute_priority's atype_score switch
      double cost = 0.5;
      switch (segments[i].access_type) {
        case AccessType::Aggregate:   cost = 1.0; break;
        case AccessType::HashBuild:   cost = 0.8; break;
        case AccessType::PointLookup: cost = 0.7; break;
        case AccessType::HashProbe:   cost = 0.6; break;
        case AccessType::Scan:        cost = 0.5; break;
        case AccessType::Sort:        cost = 0.4; break;
        case AccessType::Unknown:     cost = 0.5; break;
      }
      freq_w[i] = static_cast<uint64_t>(cost * static_cast<double>(segments[i].access_rate));
      if (freq_w[i] > max_freq_w) max_freq_w = freq_w[i];
    }
    for (size_t i = 0; i < segments.size(); ++i) {
      priorities.push_back(compute_priority(AccessType::Unknown, freq_w[i],
                                            segments[i].data_volume, segments[i].is_immutable,
                                            max_freq_w, max_volume));
    }
  } else {
    for (const auto& seg : segments) {
      priorities.push_back(compute_priority(seg.access_type, seg.access_rate, seg.data_volume,
                                             seg.is_immutable, max_rate, max_volume));
    }
  }

  // ── EDBT v4 Phase B (commit B2): shared deterministic strict-fit mapper ────
  // The ONE mapper for every migration policy (our_full, our_freq/--ablate_d1
  // a.k.a. our_no_plan, and from commit B3 cost_c3_greedy): comparator order is
  // score descending, then table name / chunk id / column id ascending; a
  // segment is selected for DRAM only when its logical byte size fits the
  // remaining budget (strict fit — oversized segments are skipped, later
  // smaller ones may still fit). This replaces the v3 capacity-accumulation
  // prefix, whose boundary segment could push selected bytes past the budget.
  const uint64_t dram_capacity = _local_resource ? _local_resource->capacity() : 0;
  const auto sel = strict_fit_map(segments, priorities, dram_capacity);
  const auto& indices = sel.order;  // rank r ↔ indices[r]

  // ── 副本策略: 所有热段(DRAM中被选中的段)均在CXL中维护副本 ──────
  // 热段 = strict-fit mapper 本周期选中的段（sel.selected）
  // 在CXL中保留副本以加速demote路径(DRAM→CXL零拷贝)
  auto replica_keep_set = std::unordered_set<SegmentKey, SegmentKeyHash>{};
  for (auto idx = size_t{0}; idx < segments.size(); ++idx) {
    if (!sel.selected[idx]) continue;
    const auto& seg = segments[idx];
    replica_keep_set.insert({seg.table_name, seg.chunk_id, seg.column_id});
  }

  // Promote: 热段中当前在 CXL 的 → promote 到 DRAM（按比较器序，最高分优先）
  // promote时进行实际数据拷贝(CXL→DRAM), 并保留CXL中的副本
  auto promoted_this_cycle = size_t{0};
  for (auto r = size_t{0}; r < indices.size(); ++r) {
    if (!sel.selected[indices[r]]) continue;
    const auto& seg = segments[indices[r]];
    if (seg.current_tier != Tier::CXL) continue;  // 已在 DRAM，无需迁移
    if (promote_segment(seg.table_name, seg.chunk_id, seg.column_id)) {
      ++promoted_this_cycle;
      ++_daemon_promote_count;
    }
  }

  // Demote: 冷段中当前在 DRAM 的 → demote 到 CXL（按比较器逆序，从最冷开始）
  // demote时利用已有CXL副本只修改映射(零拷贝), 释放DRAM中的副本
  auto demoted_this_cycle = size_t{0};
  for (auto r = indices.size(); r > 0; --r) {
    const auto idx = indices[r - 1];
    if (sel.selected[idx]) continue;
    const auto& seg = segments[idx];
    if (seg.current_tier != Tier::Local) continue;  // 已在 CXL，无需迁移
    if (demote_segment(seg.table_name, seg.chunk_id, seg.column_id)) {
      ++demoted_this_cycle;
      ++_daemon_demote_count;
    }
  }

  // ── EDBT v4 Phase B (B2): observation-only decision trace ──────────────────
  // One row per considered segment per cycle (schemas/decision_trace.schema.json).
  // Selection shown is the shared deterministic strict-fit mapper: rows are in
  // comparator order (score desc, then table/chunk/column asc); a row has
  // selected=true only when its bytes fit remaining_budget_before, so selected
  // bytes never exceed the budget (no boundary-overshoot rows remain at this
  // commit). Inert when --placement_trace is off.
  if (_trace_on) {
    const auto policy = std::string{_c3_greedy ? "cost_c3_greedy"
                                               : (_ablate_d1 ? "our_no_plan" : "our_full")};
    const auto phase = trace_phase_label();
    auto ts = timespec{};
    clock_gettime(CLOCK_REALTIME, &ts);
    const auto unix_ts_ns =
        static_cast<uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<uint64_t>(ts.tv_nsec);

    // Effective priority inputs of the active arm: wfreq replaces the rate and
    // neutralizes the atype term exactly like compute_priority's callers do;
    // c3 normalizes the raw four-pattern total (its cost model input).
    const auto max_rate_used = _c3_greedy ? max_c3_pattern_total : (_wfreq ? max_freq_w : max_rate);
    const auto atype_name = [](AccessType t) -> const char* {
      switch (t) {
        case AccessType::Aggregate:   return "Aggregate";
        case AccessType::HashBuild:   return "HashBuild";
        case AccessType::PointLookup: return "PointLookup";
        case AccessType::HashProbe:   return "HashProbe";
        case AccessType::Scan:        return "Scan";
        case AccessType::Sort:        return "Sort";
        case AccessType::Unknown:     return "Unknown";
      }
      return "Unknown";
    };
    const auto atype_score_of = [](AccessType t) -> double {
      switch (t) {
        case AccessType::Aggregate:   return 1.0;
        case AccessType::HashBuild:   return 0.8;
        case AccessType::PointLookup: return 0.7;
        case AccessType::HashProbe:   return 0.6;
        case AccessType::Scan:        return 0.5;
        case AccessType::Sort:        return 0.4;
        case AccessType::Unknown:     return 0.5;
      }
      return 0.5;
    };
    const auto fmt_double = [](double v) {
      auto out = std::array<char, 40>{};
      std::snprintf(out.data(), out.size(), "%.17g", v);
      return std::string{out.data()};
    };
    const auto dtype_name_of = [](DataType t) -> const char* {
      switch (t) {
        case DataType::Int:    return "int";
        case DataType::Long:   return "long";
        case DataType::Float:  return "float";
        case DataType::Double: return "double";
        case DataType::String: return "string";
        case DataType::Null:   return "null";
      }
      return "null";
    };
    const auto dtype_bytes_of = [](DataType t) -> uint64_t {
      switch (t) {
        case DataType::Int:    return 4;
        case DataType::Long:   return 8;
        case DataType::Float:  return 4;
        case DataType::Double: return 8;
        case DataType::String: return 16;  // sizeof(pmr_string); variable-length payload excluded
        case DataType::Null:   return 8;
      }
      return 8;
    };

    auto& trace_sm = Hyrise::get().storage_manager;
    auto buf = std::string{};
    buf.reserve(segments.size() * 224);
    for (auto r = size_t{0}; r < indices.size(); ++r) {
      const auto idx = indices[r];
      const auto& seg = segments[idx];
      // Strict-fit audit state straight from the shared mapper: remaining
      // budget before this rank, and selection iff bytes fit that remainder.
      const auto selected = sel.selected[idx] != 0;
      const auto before = sel.remaining_budget_before[r];
      const auto fits = seg.data_volume <= before;
      const auto after = selected ? before - seg.data_volume : before;

      const char* action;
      const char* target;
      if (selected) {
        if (seg.current_tier == Tier::CXL) {
          action = "promote";
          target = "DRAM";
        } else {
          action = "keep";
          target = "UNCHANGED";
        }
      } else if (before > 0) {
        // bytes > remaining > 0: the strict-fit mapper skipped this segment
        // for SIZE while budget still remained (schema: skip_no_fit). The
        // mapper's verdict is "not in the DRAM set" ⇒ CXL side.
        action = "skip_no_fit";
        target = "CXL";
      } else if (seg.current_tier == Tier::Local) {
        action = "demote";
        target = "CXL";
      } else {
        action = "keep";
        target = "UNCHANGED";
      }

      // Features exactly as evaluated by compute_priority (same expressions).
      // c3 rows keep the semantic columns as descriptive fields (照实), with
      // rate = raw four-pattern total; total_score for c3 is the C3 benefit
      // (validated as cost_cxl − cost_dram, not as the semantic term sum).
      const auto c3_pattern_total = seg.c3_seq + seg.c3_monotonic + seg.c3_random + seg.c3_point;
      const auto atype = _wfreq ? AccessType::Unknown : seg.access_type;
      const auto rate_raw = _c3_greedy ? c3_pattern_total : (_wfreq ? freq_w[idx] : seg.access_rate);
      const auto norm_freq = max_rate_used > 0 ? static_cast<double>(rate_raw) / max_rate_used : 0.0;
      const auto norm_volume = max_volume > 0 ? 1.0 - static_cast<double>(seg.data_volume) / max_volume : 0.5;
      const auto a_score = atype_score_of(atype);
      const auto imm_score = seg.is_immutable ? 1.0 : 0.0;

      const auto dtype = trace_sm.has_table(seg.table_name)
                             ? trace_sm.get_table(seg.table_name)->column_data_type(seg.column_id)
                             : DataType::Null;
      const auto card_it = _trace_cardinality.find({seg.table_name, seg.chunk_id, seg.column_id});
      const auto card = card_it == _trace_cardinality.end() ? 0.0 : static_cast<double>(card_it->second);

      buf += _trace_run_id;
      buf += "," + _trace_config + "," + std::to_string(_trace_rep) + "," + std::to_string(unix_ts_ns) +
             "," + std::to_string(_daemon_cycles) + "," + phase + "," + seg.table_name + "," +
             std::to_string(seg.chunk_id) + "," + std::to_string(seg.column_id) + "," +
             std::to_string(seg.data_volume) + "," + (seg.current_tier == Tier::CXL ? "CXL" : "DRAM") + "," +
             target + "," + std::string(action) + "," + (selected ? "true" : "false") + "," + std::to_string(r) +
             "," + (fits ? "true" : "false") + "," + policy + "," + dtype_name_of(dtype) + "," +
             std::to_string(dtype_bytes_of(dtype)) + "," + fmt_double(priorities[idx]) + "," +
             std::to_string(rate_raw) + "," + std::to_string(max_rate_used) + "," + fmt_double(norm_freq) + "," +
             atype_name(atype) + "," + fmt_double(a_score) + "," + std::to_string(seg.data_volume) + "," +
             std::to_string(max_volume) + "," + fmt_double(norm_volume) + "," +
             (seg.is_immutable ? "true" : "false") + "," + fmt_double(imm_score) + "," +
             fmt_double(kPlacementWAtype) + "," + fmt_double(kPlacementWFreq) + "," +
             fmt_double(kPlacementWVolume) + "," + fmt_double(kPlacementWImmutable) + "," +
             fmt_double(kPlacementWAtype * a_score) + "," + fmt_double(kPlacementWFreq * norm_freq) + "," +
             fmt_double(kPlacementWVolume * norm_volume) + "," + fmt_double(kPlacementWImmutable * imm_score) +
             "," + fmt_double(card) + "," + _plan_analyzer.operator_mask(seg.table_name, seg.column_id) + "," +
             std::to_string(before) + "," + std::to_string(after);
      // [EDBT v4 B2] typed-counter tail columns for ALL policies: every row
      // carries the five raw cumulative pattern counts (harness computes
      // per-cycle deltas from them). cost_c3_greedy additionally reports the
      // two cost predictions and dictionary_handling=excluded_reported (the
      // C3 cost model excludes Dictionary). All other arms are observation-
      // only here: dictionary_handling=observed_not_scored, costs empty —
      // the counts never feed those arms' priorities.
      buf += "," + std::to_string(seg.c3_seq) + "," + std::to_string(seg.c3_monotonic) + "," +
             std::to_string(seg.c3_random) + "," + std::to_string(seg.c3_point) + "," +
             std::to_string(seg.c3_dictionary) + "," +
             (_c3_greedy ? std::string("excluded_reported") : std::string("observed_not_scored")) +
             (_c3_greedy ? "," + fmt_double(seg.c3_cost_dram) + "," + fmt_double(seg.c3_cost_cxl)
                         : ",,") +
             "\n";
    }
    auto ofs = std::ofstream(_decision_trace_path, std::ios::app);
    ofs << buf;
  }


  // GC: 回收不属于热段(DRAM)的CXL副本（迁移后同步回收）
  // demote后, 被demote段的CXL副本已成为主拷贝(不再需要作为副本保留)
  auto gc_count = size_t{0};
  if (!_ablate_d3_replica) {
    for (auto it = _cxl_replicas.begin(); it != _cxl_replicas.end();) {
      if (replica_keep_set.count(it->first) == 0) {
        it = _cxl_replicas.erase(it);
        ++gc_count;
      } else {
        ++it;
      }
    }
    _daemon_gc_count += gc_count;
  }

  if (promoted_this_cycle > 0 || demoted_this_cycle > 0 || gc_count > 0) {
    std::cout << "- [Daemon] Cycle " << _daemon_cycles
              << ": promoted=" << promoted_this_cycle
              << ", demoted=" << demoted_this_cycle
              << ", gc=" << gc_count
              << " (total promotes=" << _daemon_promote_count
              << ", demotes=" << _daemon_demote_count
              << ", gc=" << _daemon_gc_count
              << ", replicas=" << _cxl_replicas.size() << ")\n";
  }

  append_daemon_metrics_row(access_delta_local, access_delta_cxl, snap_promotes, snap_demotes,
                            snap_bytes_promoted, snap_bytes_demoted, cpu_t0, cycle_wall_start);
}

// ─────────────────────────────────────────────────────────────────────────────
// EDBT v4 Phase B (commit B3): SHA-256 (FIPS 180-4) + calibration loader
// ─────────────────────────────────────────────────────────────────────────────
namespace {

// Compact local SHA-256 so the plugin can print the calibration digest in the
// BASELINE startup line without external dependencies.
class Sha256 {
 public:
  Sha256() { reset(); }

  void reset() {
    _state = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    _length = 0;
    _buffer_size = 0;
  }

  void update(const uint8_t* data, const size_t size) {
    _length += size;
    auto remaining = size;
    while (remaining > 0) {
      const auto take = std::min(remaining, size_t{64} - _buffer_size);
      std::memcpy(_buffer + _buffer_size, data, take);
      _buffer_size += take;
      data += take;
      remaining -= take;
      if (_buffer_size == 64) {
        compress(_buffer);
        _buffer_size = 0;
      }
    }
  }

  std::array<uint8_t, 32> digest() {
    const auto bit_length = _length * 8;
    const auto one = uint8_t{0x80};
    update(&one, 1);
    auto zero = uint8_t{0};
    while (_buffer_size != 56) update(&zero, 1);
    auto length_be = std::array<uint8_t, 8>{};
    for (auto i = 0u; i < 8; ++i) length_be[i] = static_cast<uint8_t>(bit_length >> (56 - 8 * i));
    update(length_be.data(), 8);
    auto out = std::array<uint8_t, 32>{};
    for (auto i = 0u; i < 8; ++i) {
      out[i * 4 + 0] = static_cast<uint8_t>(_state[i] >> 24);
      out[i * 4 + 1] = static_cast<uint8_t>(_state[i] >> 16);
      out[i * 4 + 2] = static_cast<uint8_t>(_state[i] >> 8);
      out[i * 4 + 3] = static_cast<uint8_t>(_state[i]);
    }
    return out;
  }

 private:
  static uint32_t rotr(const uint32_t value, const uint32_t bits) {
    return (value >> bits) | (value << (32 - bits));
  }

  void compress(const uint8_t* block) {
    static constexpr auto k = std::array<uint32_t, 64>{
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    auto w = std::array<uint32_t, 64>{};
    for (auto i = 0u; i < 16; ++i) {
      w[i] = (uint32_t{block[i * 4]} << 24) | (uint32_t{block[i * 4 + 1]} << 16) |
             (uint32_t{block[i * 4 + 2]} << 8) | uint32_t{block[i * 4 + 3]};
    }
    for (auto i = 16u; i < 64; ++i) {
      const auto s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const auto s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    auto a = _state[0];
    auto b = _state[1];
    auto c = _state[2];
    auto d = _state[3];
    auto e = _state[4];
    auto f = _state[5];
    auto g = _state[6];
    auto h = _state[7];
    for (auto i = 0u; i < 64; ++i) {
      const auto s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
      const auto ch = (e & f) ^ (~e & g);
      const auto temp1 = h + s1 + ch + k[i] + w[i];
      const auto s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
      const auto maj = (a & b) ^ (a & c) ^ (b & c);
      const auto temp2 = s0 + maj;
      h = g;
      g = f;
      f = e;
      e = d + temp1;
      d = c;
      c = b;
      b = a;
      a = temp1 + temp2;
    }
    _state[0] += a; _state[1] += b; _state[2] += c; _state[3] += d;
    _state[4] += e; _state[5] += f; _state[6] += g; _state[7] += h;
  }

  std::array<uint32_t, 8> _state;
  uint64_t _length = 0;
  uint8_t _buffer[64];
  size_t _buffer_size = 0;
};

std::string sha256_hex(const std::string& bytes) {
  auto sha = Sha256{};
  sha.update(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size());
  const auto digest = sha.digest();
  auto hex = std::string{};
  hex.reserve(64);
  static constexpr auto kHex = "0123456789abcdef";
  for (const auto byte : digest) {
    hex += kHex[byte >> 4];
    hex += kHex[byte & 0x0f];
  }
  return hex;
}

}  // namespace

bool SemanticPlacementPlugin::load_c3_calibration(const std::string& path) {
  auto in = std::ifstream(path, std::ios::binary);
  if (!in) {
    std::cerr << "- [B3] cannot open calibration file: " << path << "\n";
    return false;
  }
  auto bytes = std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  _c3_calibration_sha256 = sha256_hex(bytes);

  // Expected columns: device,gamma,data_type,bytes_per_value,ns_per_access_per_byte
  static const auto kDevices = std::set<std::string>{"dram", "cxl"};
  static const auto kGammas = std::set<std::string>{"sequential", "monotonic", "random", "point"};
  static const auto kDtypes = std::array<const char*, 5>{"int32", "int64", "float32", "float64", "string16"};

  _c3_calibration.clear();
  auto rows = size_t{0};
  auto stream = std::istringstream{bytes};
  auto line = std::string{};
  auto header_seen = false;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    auto cols = std::vector<std::string>{};
    auto col_stream = std::istringstream{line};
    auto token = std::string{};
    while (std::getline(col_stream, token, ',')) cols.push_back(token);
    if (!header_seen) {
      if (cols.size() < 5 || cols[0] != "device" || cols[1] != "gamma" || cols[2] != "data_type" ||
          cols[3] != "bytes_per_value" || cols[4] != "ns_per_access_per_byte") {
        std::cerr << "- [B3] calibration header mismatch: " << line << "\n";
        return false;
      }
      header_seen = true;
      continue;
    }
    if (cols.size() < 5) {
      std::cerr << "- [B3] calibration short row: " << line << "\n";
      return false;
    }
    if (!kDevices.count(cols[0]) || !kGammas.count(cols[1])) {
      std::cerr << "- [B3] calibration unknown device/gamma: " << line << "\n";
      return false;
    }
    const auto rate = std::strtod(cols[4].c_str(), nullptr);
    if (!std::isfinite(rate) || rate <= 0.0) {
      std::cerr << "- [B3] calibration invalid ns_per_access_per_byte: " << line << "\n";
      return false;
    }
    _c3_calibration[cols[0]][cols[1]][cols[2]] = rate;
    ++rows;
  }
  if (!header_seen || rows == 0) {
    std::cerr << "- [B3] calibration file has no data rows: " << path << "\n";
    return false;
  }
  // Verify the complete device × gamma × data_type grid so a missing cell can
  // never silently degrade the cost model.
  for (const auto& device : kDevices) {
    for (const auto& gamma : kGammas) {
      for (const auto* dtype : kDtypes) {
        if (!_c3_calibration.count(device) || !_c3_calibration.at(device).count(gamma) ||
            !_c3_calibration.at(device).at(gamma).count(dtype)) {
          std::cerr << "- [B3] calibration grid incomplete: missing " << device << "/" << gamma << "/"
                    << dtype << "\n";
          return false;
        }
      }
    }
  }
  std::cout << "- [B3] calibration loaded: " << path << " sha256=" << _c3_calibration_sha256
            << " rows=" << rows << " (grid 2 devices x 4 gammas x 5 dtypes complete)\n";
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// EDBT v4 Phase B (commit B1): observation-only trace setup
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::init_trace(const std::string& json_path) {
  constexpr auto dot_json_length = 5u;  // length of ".json"
  const auto stem = json_path.substr(0, json_path.size() - dot_json_length);
  _decision_trace_path = stem + "_decision_trace.csv";
  _operator_trace_path = stem + "_operator_segment_trace.csv";
  _trace_config = _c3_greedy ? "cost_c3_greedy" : (_ablate_d1 ? "our_freq" : "our_full");

  // Run identity: harness-controlled env with JSON-stem / rep-1 defaults.
  if (const auto* env_run_id = std::getenv("EDBT_V4_RUN_ID")) {
    _trace_run_id = env_run_id;
  }
  if (_trace_run_id.empty()) {
    const auto last_slash = stem.find_last_of('/');
    _trace_run_id = last_slash == std::string::npos ? stem : stem.substr(last_slash + 1);
  }
  if (const auto* env_rep = std::getenv("EDBT_V4_REP")) {
    _trace_rep = static_cast<uint32_t>(std::strtoul(env_rep, nullptr, 10));
  }

  {
    auto ofs = std::ofstream(_decision_trace_path);
    ofs << "run_id,config,rep,unix_ts_ns,cycle_id,phase,table_name,chunk_id,column_id,"
           "segment_bytes,source_tier,target_tier,action,selected,rank,fits_remaining,"
           "policy,data_type,bytes_per_value,total_score,access_rate_raw,max_rate,norm_freq,"
           "access_type,atype_score,data_volume_bytes,max_volume_bytes,norm_volume,is_immutable,"
           "immutable_score,weight_atype,weight_freq,weight_volume,weight_immutable,term_atype,"
           "term_freq,term_volume,term_immutable,estimated_cardinality,operator_mask,"
           "remaining_budget_before,remaining_budget_after,sequential_count,monotonic_count,"
           "random_count,point_count,dictionary_count,dictionary_handling,cost_dram,cost_cxl\n";
  }
  std::cout << "- [EDBT-TRACE] ON run_id=" << _trace_run_id << " config=" << _trace_config
            << " rep=" << _trace_rep << " decision_trace=" << _decision_trace_path << "\n";
}

std::string SemanticPlacementPlugin::trace_phase_label() const {
  const auto start = Hyrise::get().timed_phase_start_steady_ns.load(std::memory_order_relaxed);
  if (start < 0) return "warmup";
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count();
  const auto planned = Hyrise::get().timed_phase_planned_duration_ns.load(std::memory_order_relaxed);
  return static_cast<uint64_t>(now - start) < planned / 2 ? "timed_first_half" : "timed_second_half";
}

// ─────────────────────────────────────────────────────────────────────────────
// [EDBT v4 B2] Single-client measured diagnostic pass (default OFF).
// Runs between the initial placement and the daemon start, strictly sequential
// (JOB CLI gates --placement_trace_measured to --clients 1), so a counter
// delta around one query is a true measurement for that execution window.
// PQP/benchmark metric availability inspected for this pass:
//   * AbstractBenchmarkItemRunner::execute_item returns statement-level
//     SQLPipelineMetrics only (no per-operator data) → not used here.
//   * SQLPipeline::get_physical_plans() after execution exposes every
//     operator's performance_data: walltime, output_row_count,
//     output_chunk_count (measured). rows_in is derived as the input
//     operator's output_row_count. There is NO per-operator segment access
//     metric in this fork → operator-level actual_reads is filled only via
//     the unique-scan attribution rule below, never fabricated.
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::run_measured_diagnostic_pass() {
  using SAC = SegmentAccessCounter::AccessType;
  auto& storage_manager = Hyrise::get().storage_manager;

  const auto query_dir = std::filesystem::path(_plan_analyzer.query_dir());
  if (!std::filesystem::exists(query_dir)) {
    std::cout << "- [EDBT-MEASURED] diagnostic pass skipped (query dir not found: " << query_dir
              << ")\n";
    return;
  }
  auto sql_files = std::vector<std::filesystem::path>{};
  for (const auto& entry : std::filesystem::directory_iterator(query_dir)) {
    if (!entry.is_regular_file()) continue;
    const auto& fpath = entry.path();
    if (fpath.extension() != ".sql") continue;
    const auto stem = fpath.stem().string();
    if (stem == "schema" || stem == "fkindexes") continue;
    sql_files.push_back(fpath);
  }
  std::sort(sql_files.begin(), sql_files.end());

  _measured_segment_access_path = _decision_trace_path.substr(0, _decision_trace_path.size() -
                                                                     std::string("_decision_trace.csv").size()) +
                                  "_measured_segment_access.csv";
  std::ofstream measured_ofs(_measured_segment_access_path);
  measured_ofs << "run_id,config,rep,query_name,execution_id,table_name,chunk_id,column_id,"
                  "sequential_delta,monotonic_delta,random_delta,point_delta,dictionary_delta,"
                  "total_delta,tier_at_execution,attribution_level\n";
  auto operator_ofs = std::ofstream(_operator_trace_path, std::ios::app);
  // Per-operator wall/rows diagnostics (B2-5): written for EVERY TableScan of
  // every executed query. Operator-level duration/row metrics are reliable per
  // execution, but they carry NO segment-read attribution — the operator
  // segment trace only accepts rows whose actual_reads is filled (validator
  // contract: attribution_level=measured_per_execution requires actual_reads),
  // so this sidecar keeps the complete operator duration/rows record without
  // any attribution claim.
  const auto wall_rows_path = _decision_trace_path.substr(0, _decision_trace_path.size() -
                                                                        std::string("_decision_trace.csv").size()) +
                               "_operator_wall_rows.csv";
  std::ofstream wall_ofs(wall_rows_path);
  wall_ofs << "run_id,config,rep,query_name,execution_id,operator_index,operator_name,"
              "table_name,column_id,rows_in,rows_out,wall_ns,unique_scan,n_chunks_touched\n";

  // Snapshot every segment's five pattern counters (cumulative values).
  const auto snapshot_counters = [&]() {
    auto snap = std::unordered_map<SegmentKey, std::array<uint64_t, 5>, SegmentKeyHash>{};
    snap.reserve(8192);
    for (const auto& [table_name, table] : storage_manager.tables()) {
      const auto chunk_count = table->chunk_count();
      const auto column_count = table->column_count();
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        const auto& chunk = table->get_chunk(chunk_id);
        if (!chunk) continue;
        for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
          const auto& segment = chunk->get_segment(col_id);
          if (!segment) continue;
          const auto& counter = segment->access_counter;
          snap[SegmentKey{table_name, chunk_id, col_id}] = {
              counter[SAC::Sequential].load(std::memory_order_relaxed),
              counter[SAC::Monotonic].load(std::memory_order_relaxed),
              counter[SAC::Random].load(std::memory_order_relaxed),
              counter[SAC::Point].load(std::memory_order_relaxed),
              counter[SAC::Dictionary].load(std::memory_order_relaxed)};
        }
      }
    }
    return snap;
  };

  const auto tier_label = [&](const SegmentKey& key) -> const char* {
    const auto it = _placement_map.find(key);
    if (it == _placement_map.end()) return "UNKNOWN";
    return it->second == Tier::CXL ? "CXL" : "DRAM";
  };

  // First PQPColumnExpression found in an expression tree (scan predicate column).
  const auto find_pqp_column = [](const std::shared_ptr<AbstractExpression>& expression) {
    std::function<const PQPColumnExpression*(const std::shared_ptr<AbstractExpression>&)> walk =
        [&](const std::shared_ptr<AbstractExpression>& expr) -> const PQPColumnExpression* {
      if (const auto* column = dynamic_cast<const PQPColumnExpression*>(expr.get())) return column;
      for (const auto& argument : expr->arguments) {
        if (const auto* column = walk(argument)) return column;
      }
      return nullptr;
    };
    return walk(expression);
  };

  auto measured_rows = size_t{0};
  auto operator_rows = size_t{0};
  auto wall_rows = size_t{0};
  for (const auto& sql_file : sql_files) {
    auto ifstream = std::ifstream(sql_file);
    if (!ifstream.is_open()) continue;
    auto sql = std::string{};
    auto line = std::string{};
    while (std::getline(ifstream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      sql += line + "\n";
    }
    if (sql.empty()) continue;
    const auto query_name = sql_file.stem().string();

    const auto before = snapshot_counters();
    auto pqp_roots = std::vector<std::shared_ptr<AbstractOperator>>{};
    auto execution_ok = true;
    try {
      auto pipeline = SQLPipelineBuilder{sql}.create_pipeline();
      const auto [status, result_table] = pipeline.get_result_table();  // executes; PQPs keep performance data
      (void)result_table;
      if (status != SQLPipelineStatus::Success) {
        std::cout << "- [EDBT-MEASURED] warning: " << query_name << " pipeline status "
                  << static_cast<int>(status) << "\n";
        execution_ok = false;
      } else {
        pqp_roots = pipeline.get_physical_plans();
      }
    } catch (const std::exception& e) {
      std::cout << "- [EDBT-MEASURED] warning: " << query_name << " failed: " << e.what() << "\n";
      execution_ok = false;
    }
    const auto after = snapshot_counters();

    if (!execution_ok) continue;

    // Per-query measured counter deltas (exact: single client, sequential).
    struct MeasuredDelta {
      std::array<uint64_t, 5> patterns{};
      uint64_t total = 0;
    };
    auto deltas = std::unordered_map<SegmentKey, MeasuredDelta, SegmentKeyHash>{};
    for (const auto& [key, after_counts] : after) {
      const auto it = before.find(key);
      if (it == before.end()) continue;
      auto delta = MeasuredDelta{};
      for (auto p = size_t{0}; p < 5; ++p) {
        delta.patterns[p] = after_counts[p] >= it->second[p] ? after_counts[p] - it->second[p] : 0;
        delta.total += delta.patterns[p];
      }
      if (delta.total > 0) deltas.emplace(key, delta);
    }
    for (const auto& [key, delta] : deltas) {
      measured_ofs << _trace_run_id << "," << _trace_config << "," << _trace_rep << "," << query_name
                   << ",0," << key.table_name << "," << key.chunk_id << "," << key.column_id << ","
                   << delta.patterns[0] << "," << delta.patterns[1] << "," << delta.patterns[2] << ","
                   << delta.patterns[3] << "," << delta.patterns[4] << "," << delta.total << ","
                   << tier_label(key) << ",measured_per_execution\n";
      ++measured_rows;
    }

    // Operator rows: one per TableScan over a stored table (the memory-touching
    // operator class). Collect scans per (table, column); a scan's actual_reads
    // (measured segment delta) is attributable ONLY when exactly one scan of
    // this query references that (table, column). Operator-trace rows exist
    // only for those unique scans (validator contract: measured rows require
    // actual_reads); every scan's wall/rows goes to the wall-rows sidecar.
    struct ScanRef {
      std::shared_ptr<AbstractOperator> scan;
      std::shared_ptr<AbstractOperator> input;   // operator directly beneath (rows_in)
      std::string table_name;
      std::vector<ChunkID> pruned_chunks;
    };
    auto scans = std::vector<ScanRef>{};
    auto column_of_scan = std::vector<std::optional<ColumnID>>{};

    std::function<void(const std::shared_ptr<AbstractOperator>&)> collect_pqp =
        [&](const std::shared_ptr<AbstractOperator>& op) {
          if (!op) return;
          if (op->name() == "TableScan") {
            const auto scan = std::dynamic_pointer_cast<TableScan>(op);
            const auto* column = scan ? find_pqp_column(scan->predicate()) : nullptr;
            // Descend to the stored table beneath (through stacked scans / Validate).
            auto base = scan ? scan->mutable_left_input() : nullptr;
            while (base && (base->name() == "TableScan" || base->name() == "Validate")) {
              base = base->mutable_left_input();
            }
            if (column && base && base->name() == "GetTable") {
              const auto get_table = std::dynamic_pointer_cast<GetTable>(base);
              if (get_table) {
                scans.push_back(ScanRef{scan, scan->mutable_left_input(), get_table->table_name(),
                                        get_table->pruned_chunk_ids()});
                column_of_scan.push_back(column->column_id);
              }
            }
          }
          collect_pqp(op->mutable_left_input());
          collect_pqp(op->mutable_right_input());
        };
    for (const auto& root : pqp_roots) collect_pqp(root);

    // Count references per (table, column) across this query's scans.
    auto reference_count = std::map<std::pair<std::string, ColumnID>, size_t>{};
    for (auto i = size_t{0}; i < scans.size(); ++i) {
      ++reference_count[{scans[i].table_name, column_of_scan[i].value()}];
    }

    for (auto i = size_t{0}; i < scans.size(); ++i) {
      const auto& ref = scans[i];
      const auto column_id = column_of_scan[i].value();
      const auto unique = reference_count[{ref.table_name, column_id}] == 1;

      const auto& scan_perf = *ref.scan->performance_data;
      const auto rows_out = scan_perf.has_output ? scan_perf.output_row_count : 0;
      auto rows_in = uint64_t{0};
      if (ref.input && ref.input->performance_data->has_output) {
        rows_in = ref.input->performance_data->output_row_count;
      }
      const auto wall_ns = static_cast<uint64_t>(scan_perf.walltime.count());

      const auto pruned = std::set<ChunkID>(ref.pruned_chunks.begin(), ref.pruned_chunks.end());
      if (!storage_manager.has_table(ref.table_name)) continue;
      const auto chunk_count = storage_manager.get_table(ref.table_name)->chunk_count();
      auto chunks_touched = size_t{0};
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        if (!pruned.count(chunk_id)) ++chunks_touched;
      }
      // Every scan's measured duration/rows — no attribution claimed.
      wall_ofs << _trace_run_id << "," << _trace_config << "," << _trace_rep << "," << query_name
               << ",0," << i << ",TableScan," << ref.table_name << "," << column_id << ","
               << rows_in << "," << rows_out << "," << wall_ns << ","
               << (unique ? "true" : "false") << "," << chunks_touched << "\n";
      ++wall_rows;
      if (!unique) continue;  // non-unique scans: no measured operator-trace rows
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        if (pruned.count(chunk_id)) continue;
        const SegmentKey key{ref.table_name, chunk_id, column_id};
        // unique-scan rule: the measured delta of this (table, column) is
        // attributable to this scan. A missing delta entry is a measured ZERO
        // (the deltas map only stores total > 0), never an unknown — write 0.
        auto actual_reads = std::string{"0"};
        const auto it = deltas.find(key);
        if (it != deltas.end()) actual_reads = std::to_string(it->second.total);
        operator_ofs << _trace_run_id << "," << _trace_config << "," << _trace_rep << "," << query_name
                     << ",0," << i << ",TableScan," << ref.table_name << "," << chunk_id << ","
                     << column_id << ",0," << rows_in << "," << rows_out << "," << wall_ns << ","
                     << actual_reads << "," << tier_label(key) << ",measured_per_execution,,,,\n";
        ++operator_rows;
      }
    }
  }

  std::cout << "- [EDBT-MEASURED] diagnostic pass: " << measured_rows
            << " measured segment-delta rows, " << operator_rows
            << " measured operator rows (unique-scan only), " << wall_rows
            << " operator wall/rows rows -> " << _measured_segment_access_path << " / "
            << wall_rows_path << "\n";

  // Re-sync the daemon's baseline snapshots: the measured queries themselves
  // bumped the cumulative counters, and daemon cycle 1's deltas must cover
  // benchmark accesses only (same warm-start semantics as pre_benchmark_hook
  // step 7 — cumulative counter as EMA seed / last-access baseline).
  for (const auto& [table_name, table] : storage_manager.tables()) {
    const auto chunk_count = table->chunk_count();
    const auto column_count = table->column_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      if (!chunk) continue;
      for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
        const auto& segment = chunk->get_segment(col_id);
        if (!segment) continue;
        const SegmentKey key{table_name, chunk_id, col_id};
        const auto cumulative = get_segment_access_frequency(segment);
        _last_access[key] = cumulative;
        _ema_freq[key] = static_cast<double>(cumulative);
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// P0-1/P0-2 (plan v2 §4): append one daemon_metrics.csv row for this cycle
// ─────────────────────────────────────────────────────────────────────────────
void SemanticPlacementPlugin::append_daemon_metrics_row(uint64_t access_delta_local,
                                                        uint64_t access_delta_cxl,
                                                        size_t snap_promotes, size_t snap_demotes,
                                                        uint64_t snap_bytes_promoted,
                                                        uint64_t snap_bytes_demoted, timespec cpu_t0,
                                                        time_t wall_start) {
  if (_daemon_metrics_path.empty()) return;
  auto cpu_t1 = timespec{};
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_t1);
  const auto cpu_ms =
      static_cast<double>(cpu_t1.tv_sec - cpu_t0.tv_sec) * 1e3 +
      static_cast<double>(cpu_t1.tv_nsec - cpu_t0.tv_nsec) / 1e6;

  // P0-2: current staging replica footprint (CXL replicas of hot DRAM segments)
  auto replica_bytes_total = uint64_t{0};
  for (const auto& [key, seg] : _cxl_replicas) {
    replica_bytes_total += seg->memory_usage(MemoryUsageCalculationMode::Full);
  }

  const auto total_access = access_delta_local + access_delta_cxl;
  // Convention: no accesses in period → hit rate reported as 100.0 (vacuous; noted in MANIFEST)
  const auto hit_rate_pct =
      total_access > 0 ? 100.0 * static_cast<double>(access_delta_local) / total_access : 100.0;

  std::ofstream ofs(_daemon_metrics_path, std::ios::app);
  ofs << wall_start << "," << _daemon_cycles << "," << hit_rate_pct
      << "," << (_daemon_promote_count - snap_promotes)
      << "," << (_daemon_demote_count - snap_demotes)
      << "," << (_daemon_bytes_promoted - snap_bytes_promoted)
      << "," << (_daemon_bytes_demoted - snap_bytes_demoted)
      << "," << cpu_ms << "," << replica_bytes_total << "\n";
}

// ─────────────────────────────────────────────────────────────────────────────
// P(s) semantic priority function
// ─────────────────────────────────────────────────────────────────────────────
double SemanticPlacementPlugin::compute_priority(AccessType access_type, uint64_t access_rate,
                                                  uint64_t data_volume, bool is_immutable,
                                                  uint64_t max_rate, uint64_t max_volume) const {
  // Normalize access rate [0, 1] — absolute frequency, stable in steady state
  auto norm_freq = max_rate > 0 ? static_cast<double>(access_rate) / max_rate : 0.0;

  // Normalize volume: smaller = higher priority (cheaper to migrate)
  auto norm_volume = max_volume > 0 ? 1.0 - static_cast<double>(data_volume) / max_volume : 0.5;

  // Access type semantic score (LQP operator type)
  auto atype_score = 0.5;
  switch (access_type) {
    case AccessType::Aggregate:   atype_score = 1.0; break;  // CPU-bound, highest priority
    case AccessType::HashBuild:   atype_score = 0.8; break;  // latency-sensitive
    case AccessType::PointLookup: atype_score = 0.7; break;
    case AccessType::HashProbe:   atype_score = 0.6; break;
    case AccessType::Scan:        atype_score = 0.5; break;  // bandwidth-sensitive
    case AccessType::Sort:        atype_score = 0.4; break;
    case AccessType::Unknown:     atype_score = 0.5; break;
  }

  // Immutability bonus (safe to migrate, zero consistency cost)
  auto immutable_score = is_immutable ? 1.0 : 0.0;

  // Weighted sum: semantic type dominates (50%), frequency is secondary (25%).
  // [IMPL-3]: constants replaced by macro-overridable kPlacementW* (see top of file);
  // without build-time defines these are exactly the frozen v2 values.
  return kPlacementWAtype * atype_score +
         kPlacementWFreq * norm_freq +
         kPlacementWVolume * norm_volume +
         kPlacementWImmutable * immutable_score;
}

// ─────────────────────────────────────────────────────────────────────────────
// Promote a segment from CXL to Local (DRAM)
// ─────────────────────────────────────────────────────────────────────────────
bool SemanticPlacementPlugin::promote_segment(const std::string& table_name, ChunkID chunk_id,
                                               ColumnID column_id) {
  auto& storage_manager = Hyrise::get().storage_manager;
  if (!storage_manager.has_table(table_name)) return false;
  auto table = storage_manager.get_table(table_name);
  auto chunk = table->get_chunk(chunk_id);
  if (!chunk || chunk->is_mutable()) return false;
  auto segment = chunk->get_segment(column_id);
  if (!segment) return false;

  // Check DRAM capacity
  auto seg_size = segment->memory_usage(MemoryUsageCalculationMode::Full);
  if (!_local_resource || _local_resource->free_bytes() < seg_size) {
    return false;  // not enough DRAM space
  }

  SegmentKey key{table_name, chunk_id, column_id};

  // 保留CXL旧段作为副本: demote时可直接复用(零拷贝)
  if (!_ablate_d3_replica) {
    _cxl_replicas[key] = segment;
  }

  std::shared_ptr<AbstractSegment> new_segment;
  try {
    auto t0 = std::chrono::steady_clock::now();
    new_segment = segment->copy_using_memory_resource(_local_resource);
    auto t1 = std::chrono::steady_clock::now();
    _daemon_migration_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    _daemon_bytes_migrated += seg_size;
    _daemon_bytes_promoted += seg_size;  // P0-1: per-direction byte accounting
    ++_daemon_copy_count;
  } catch (...) {
    if (!_ablate_d3_replica) _cxl_replicas.erase(key);  // 分配失败, 回滚副本记录
    return false;  // DRAM buffer exhausted despite capacity check (alignment waste)
  }
  if (!new_segment) {
    if (!_ablate_d3_replica) _cxl_replicas.erase(key);
    return false;
  }
  chunk->replace_segment(column_id, new_segment);

  _placement_map[key] = Tier::Local;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Demote a segment from Local (DRAM) to CXL
// ─────────────────────────────────────────────────────────────────────────────
bool SemanticPlacementPlugin::demote_segment(const std::string& table_name, ChunkID chunk_id,
                                              ColumnID column_id) {
  auto& storage_manager = Hyrise::get().storage_manager;
  if (!storage_manager.has_table(table_name)) return false;
  auto table = storage_manager.get_table(table_name);
  auto chunk = table->get_chunk(chunk_id);
  if (!chunk || chunk->is_mutable()) return false;
  auto segment = chunk->get_segment(column_id);
  if (!segment) return false;

  SegmentKey key{table_name, chunk_id, column_id};

  // 优先利用已有CXL副本: 只修改映射关系(零拷贝), 释放DRAM段
  // (promote时保留了CXL旧段副本, demote时直接切换chunk引用即可)
  if (!_ablate_d3_replica) {
    auto it = _cxl_replicas.find(key);
    if (it != _cxl_replicas.end()) {
      chunk->replace_segment(column_id, it->second);
      _placement_map[key] = Tier::CXL;
      ++_daemon_zero_copy_count;  // 零拷贝demote: 无数据拷贝, 无带宽消耗
      return true;
    }
  }

  // 首次demote (无已有CXL副本, 如dram_first初始热段): 在CXL上分配新段并拷贝
  // 后续promote时会创建CXL副本, 之后的demote即可零拷贝
  auto seg_size = segment->memory_usage(MemoryUsageCalculationMode::Full);
  if (!_cxl_resource || _cxl_resource->free_bytes() < seg_size) {
    return false;  // not enough CXL space
  }

  std::shared_ptr<AbstractSegment> new_segment;
  try {
    auto t0 = std::chrono::steady_clock::now();
    new_segment = segment->copy_using_memory_resource(_cxl_resource);
    auto t1 = std::chrono::steady_clock::now();
    _daemon_migration_time_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    _daemon_bytes_migrated += seg_size;
    _daemon_bytes_demoted += seg_size;  // P0-1: per-direction byte accounting
    ++_daemon_copy_count;
  } catch (...) {
    return false;  // CXL buffer exhausted
  }
  if (!new_segment) return false;
  chunk->replace_segment(column_id, new_segment);

  _placement_map[key] = Tier::CXL;
  return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Read total access frequency from a segment's access counter (atomic-safe)
// ─────────────────────────────────────────────────────────────────────────────
uint64_t SemanticPlacementPlugin::get_segment_access_frequency(
    const std::shared_ptr<AbstractSegment>& segment) const {
  const auto& counter = segment->access_counter;
  return counter[SegmentAccessCounter::AccessType::Point].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Sequential].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Monotonic].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Random].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Dictionary].load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// post_benchmark_hook: stop daemon + metrics
// ─────────────────────────────────────────────────────────────────────────────
std::optional<PostBenchmarkHook> SemanticPlacementPlugin::post_benchmark_hook() {
  return [this](auto& report) {
    // 1. Stop daemon thread
    if (_daemon_thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(_daemon_mutex);
        _daemon_running = false;
      }
      _daemon_cv.notify_all();
      _daemon_thread.join();
    }

    // 2. Add metrics to JSON report
    const auto total_migration_time_ns = _daemon_migration_time_ns + _init_migration_time_ns;
    const auto total_bytes_migrated = _daemon_bytes_migrated + _init_bytes_migrated;
    const auto total_copy_count = _daemon_copy_count + _init_copy_count;
    const auto avg_migration_time_ms = total_copy_count > 0
        ? static_cast<double>(total_migration_time_ns) / static_cast<double>(total_copy_count) / 1e6
        : 0.0;
    const auto migration_bandwidth_mbs = total_migration_time_ns > 0
        ? static_cast<double>(total_bytes_migrated) / (static_cast<double>(total_migration_time_ns) / 1e9) / (1024.0 * 1024.0)
        : 0.0;

    report["semantic_placement"] = {
        {"total_decisions", _total_decisions},
        {"local_count", _local_count},
        {"cxl_count", _cxl_count},
        {"ablate_d1", _ablate_d1},
        {"ablate_d2", _ablate_d2},
        {"ablate_d3_replica", _ablate_d3_replica},
        {"dram_first", _dram_first},
        {"daemon_period_seconds", _daemon_period_seconds},
        {"daemon_cycles", _daemon_cycles},
        {"daemon_promotes", _daemon_promote_count},
        {"daemon_demotes", _daemon_demote_count},
        {"daemon_gc_count", _daemon_gc_count},
        {"cxl_replica_count", _cxl_replicas.size()},
        {"ema_alpha", EMA_ALPHA},
        {"ema_freq_tracked", _ema_freq.size()},
        {"access_type_tracked", _access_type_map.size()},
        // Migration cost metrics — daemon
        {"daemon_migration_time_ms", static_cast<double>(_daemon_migration_time_ns) / 1e6},
        {"daemon_bytes_migrated", _daemon_bytes_migrated},
        {"daemon_copy_count", _daemon_copy_count},
        {"daemon_zero_copy_count", _daemon_zero_copy_count},
        // Migration cost metrics — initial placement
        {"init_migration_time_ms", static_cast<double>(_init_migration_time_ns) / 1e6},
        {"init_bytes_migrated", _init_bytes_migrated},
        {"init_copy_count", _init_copy_count},
        // Migration cost metrics — totals + derived
        {"total_migration_time_ms", static_cast<double>(total_migration_time_ns) / 1e6},
        {"total_bytes_migrated", total_bytes_migrated},
        {"total_copy_count", total_copy_count},
        {"avg_migration_time_ms", avg_migration_time_ms},
        {"migration_bandwidth_mbs", migration_bandwidth_mbs}};
  };
}

EXPORT_PLUGIN(SemanticPlacementPlugin);

}  // namespace hyrise
