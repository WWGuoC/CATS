#include "placement_plugin.hpp"

#include <chrono>
#include <ctime>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "../benchmarklib/abstract_benchmark_item_runner.hpp"
#include "hyrise.hpp"
#include "import_export/csv/csv_writer.hpp"
#include "operators/print.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "storage/chunk.hpp"
#include "storage/segment_access_counter.hpp"
#include "storage/table.hpp"
#include "utils/assert.hpp"
#include "utils/timer.hpp"
#include "utils/string_utils.hpp"

#include "magic_enum.hpp"

namespace hyrise {

// P0-3 helper: total access count of one segment (all access types, atomic-safe)
inline uint64_t sum_segment_accesses(const std::shared_ptr<AbstractSegment>& segment) {
  const auto& counter = segment->access_counter;
  return counter[SegmentAccessCounter::AccessType::Point].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Sequential].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Monotonic].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Random].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Dictionary].load(std::memory_order_relaxed);
}

inline void create_table_access_snapshot(const char* table_name){
  std::cout << "- Create access statistics snapshot table...\n";
  auto ss = std::stringstream{};
  ss << "CREATE TABLE " << table_name << R"( AS
    SELECT
      table_name, column_name, SUM(point_accesses) AS point, SUM(sequential_accesses) AS seq, SUM(monotonic_accesses)
      AS mon, SUM(random_accesses) AS rnd, SUM(dictionary_accesses) AS dict, SUM(size_in_bytes) AS bytes
    FROM
      meta_segments_accurate
    GROUP BY table_name, column_name)";
  SQLPipelineBuilder{ss.str()}.create_pipeline().get_result_table();
}

inline void create_table_access_delta(const char* table_name, const char* table_name_before, const char* table_name_after){
  std::cout << "- Create access statistics delta table...\n";
  auto ss = std::stringstream{};
  ss << "CREATE TABLE " << table_name << " AS ";
  ss << R"(SELECT
            i.table_name AS table_name, i.column_name AS column_name, i.point AS point, i.seq AS seq, i.mon AS mon, i.rnd AS rnd, i.dict AS dict,
            (i.point + i.seq + i.mon + i.rnd + i.dict) AS total_accesses, (CAST(i.bytes AS FLOAT) / (1000000000)) AS size_GB
          FROM (
            SELECT
              "t2".table_name, "t2".column_name,
              ("t2".point - "t1".point) AS point,
              ("t2".seq - "t1".seq) AS seq,
              ("t2".mon - "t1".mon) AS mon,
              ("t2".rnd - "t1".rnd) AS rnd,
              ("t2".dict - "t1".dict) AS dict,
              "t2".bytes AS bytes
            FROM )";
  ss << table_name_after << R"( AS "t2" JOIN )" << table_name_before << R"( AS "t1" ON t2.column_name = t1.column_name)";
  ss << ") AS i ORDER BY total_accesses DESC";
  SQLPipelineBuilder{ss.str()}.create_pipeline().get_result_table();
}

inline void print_table_sizes() {
  auto ss = std::stringstream{};
  ss << R"(
    SELECT
      i.table_name, (CAST(i.bytes AS FLOAT) / (1000000000)) AS size_GB
    FROM (
      SELECT table_name, SUM(size_in_bytes) AS bytes
      FROM meta_segments_accurate
      GROUP BY table_name
    ) AS i
    WHERE i.table_name IN ('region', 'customer', 'nation', 'supplier', 'lineitem', 'part', 'orders', 'partsupp')
  )";
  Print::print(ss.str());
}

inline void print_total_tables_size() {
  auto ss = std::stringstream{};
  ss << R"(
    SELECT
      (CAST(SUM(i.bytes) AS FLOAT) / (1000000000)) AS size_GB
    FROM (
      SELECT table_name, SUM(size_in_bytes) AS bytes
      FROM meta_segments_accurate
      WHERE table_name IN ('region', 'customer', 'nation', 'supplier', 'lineitem', 'part', 'orders', 'partsupp')
      GROUP BY table_name
    ) AS i
  )";
  Print::print(ss.str());
}

inline void print_table(const std::string_view table_name){
  auto ss = std::stringstream{};
  ss << "SELECT * FROM " << table_name;
  Print::print(ss.str());
}

std::string PlacementPlugin::description() const {
  return "PlacementPlugin";
}

void PlacementPlugin::start() {
  // TODO(MW)
}

void PlacementPlugin::stop() {
  // TODO(MW) stop
}

std::vector<std::pair<PluginFunctionName, PluginFunctionPointer>> PlacementPlugin::provided_user_executable_functions() {
  return {};
}

void PlacementPlugin::run_page_placement(AbstractBenchmarkItemRunner& benchmark_item_runner) {
  std::cout << "Run page placement" << std::endl;
  auto& resources = Hyrise::get().memory_resources;
  auto resource = resources["cxl"].get();
  auto linear_resource = dynamic_cast<LinearNumaMemoryResource*>(resource);
  Assert(resource, "Cast to LinearNumaMemoryResource* failed");
  auto& storage_manager = Hyrise::get().storage_manager;

  const auto& options = Hyrise::get().placement_options;
  const auto nodes_str = values_to_string(linear_resource->node_ids());
  std::cout << "- Migrating table data with " << magic_enum::enum_name(options.type) << " accross nodes " << nodes_str << std::endl;
  auto per_table_timer = Timer{};
  for (auto& [table_name, table] : storage_manager.tables()) {
    table->migrate(linear_resource);
    std::cout << "-  Migrated '" << table_name << "' (" << per_table_timer.lap_formatted() << ")" << std::endl;
  }
}

void PlacementPlugin::run_column_placement(AbstractBenchmarkItemRunner& benchmark_item_runner) {
  std::cout << "Run column placement\n" << std::flush;
  const auto& options = Hyrise::get().placement_options;
  Assert(!options.lmem_node_ids.empty(), "lmem node ids should not be empty for column placements.");
  if (!options.lmem_node_ids.empty()) {
      // Honor explicit capacity if set (>0), else fall back to scale_factor × SIZE_GIB
      // (needed for datasets without a benchmark scale factor, e.g. JOB/IMDB where
      // scale_factor=1.0 would yield a 1 GiB buffer and overflow during migration).
      const auto local_buf = options.dram_capacity_gb > 0.0f
                                 ? static_cast<uint64_t>(options.dram_capacity_gb * SIZE_GIB)
                                 : static_cast<uint64_t>(options.scale_factor * SIZE_GIB * 1.0);
      auto memory_resource = std::make_unique<LinearNumaMemoryResource>(local_buf, options.lmem_node_ids);
      Hyrise::get().memory_resources.emplace("local", std::move(memory_resource));
  }

  // Cache table pointers before SQL operations to avoid potential StorageManager
  // hash-map corruption after mbind/numa_set_strict affects TBB allocator state.
  auto& stm = Hyrise::get().storage_manager;
  auto table_cache = std::unordered_map<std::string, std::shared_ptr<Table>>{};
  for (const auto& [name, table] : stm.tables()) {
    table_cache.emplace(std::string(name), table);
  }
  std::cout << "- Cached " << table_cache.size() << " tables for migration" << std::endl;

  constexpr auto table_name_before = "segment_accesses_before_workload";
  constexpr auto table_name_after = "segment_accesses_after_workload";
  constexpr auto table_name_delta = "segment_accesses_delta_workload";

  create_table_access_snapshot(table_name_before);
  for (const auto item_id : benchmark_item_runner.items()) {
    benchmark_item_runner.execute_item(item_id);
  }
  create_table_access_snapshot(table_name_after);
  create_table_access_delta(table_name_delta, table_name_before, table_name_after);
  ////////// COLUMN PLACEMENT
  auto ss = std::stringstream{};
  ss << "SELECT table_name, column_name, total_accesses, size_GB FROM " << table_name_delta;
  const auto [pipeline_status, access_table] = SQLPipelineBuilder{ss.str()}.create_pipeline().get_result_table();
  auto csv_path = options.json_path;
  constexpr auto dot_json_length = 5u; // length of ".json"
  csv_path = csv_path.substr(0, csv_path.size() - dot_json_length) + "_accesses.csv";
  CsvWriter::write(*access_table, csv_path);
//  Print::print(access_table);
//  print_table_sizes();
//  print_total_tables_size();

  // Aggregate column ids to migrate for each table
  // Use cached table pointers instead of stm.get_table() to avoid TBB hash-map
  // SIGSEGV that occurs after SQL operations + LinearNumaMemoryResource setup.
  auto column_ids_to_migrate = std::unordered_map<std::string, std::vector<ColumnID>>{};
  auto column_names_to_migrate = std::unordered_map<std::string, std::vector<pmr_string>>{};
  auto row_idx = 0u;
  auto rows = access_table->get_rows();
  for (const auto& row : rows) {
    const auto table_name = boost::get<pmr_string>(row[0]);
    const auto column_name = boost::get<pmr_string>(row[1]);
    if (row_idx < options.num_most_frequently_columns_local) {
      ++row_idx;
      continue;
    }
    const auto table_name_str = std::string(table_name);
    auto cache_it = table_cache.find(table_name_str);
    if (cache_it == table_cache.end()) {
      std::cerr << "- Warning: table '" << table_name_str << "' not in cache, skipping" << std::endl;
      ++row_idx;
      continue;
    }
    const auto column_id = cache_it->second->column_id_by_name(std::string(column_name));
    column_ids_to_migrate[table_name_str].push_back(column_id);
    column_names_to_migrate[table_name_str].push_back(column_name);
    ++row_idx;
  }

  // Actual migration
  auto& resources = Hyrise::get().memory_resources;
  Assert(resources.contains("cxl"), "resource for 'cxl' does not exitst");
  Assert(resources.contains("local"), "resource for 'local' does not exitst");

  auto cxl_resource = resources["cxl"].get();
  auto cxl_linear_resource = dynamic_cast<LinearNumaMemoryResource*>(cxl_resource);
  Assert(cxl_linear_resource, "Cast to LinearNumaMemoryResource* failed");

  auto local_resource = resources["local"].get();
  auto local_linear_resource = dynamic_cast<LinearNumaMemoryResource*>(local_resource);
  Assert(local_linear_resource, "Cast to LinearNumaMemoryResource* failed");

  for (auto& [table_name, column_ids] : column_ids_to_migrate) {
    auto cache_it = table_cache.find(table_name);
    if (cache_it == table_cache.end()) {
      continue;
    }
    auto table = cache_it->second;
    std::cout << "Migrating columns of table " << table_name << ": ";
    std::sort(column_ids.begin(), column_ids.end());
    for (auto& col : column_names_to_migrate[table_name]) {
      std::cout << col << ", ";
    }
    std::cout << "\n" << std::flush;
    table->migrate_columns(column_ids, cxl_linear_resource, local_linear_resource);
  }

  // ── P0-3 (plan v2 §4): start baseline access sampler (5 s interval) ───────
  {
    const auto& opt = Hyrise::get().placement_options;
    constexpr auto dot_json_length = 5u;  // length of ".json"
    _sampler_csv_path =
        opt.json_path.substr(0, opt.json_path.size() - dot_json_length) + "_daemon_metrics.csv";
    {
      std::ofstream ofs(_sampler_csv_path);
      ofs << "unix_ts,cycle_id,hit_rate_pct,promotes,demotes,bytes_promoted,bytes_demoted,"
             "daemon_cpu_ms,replica_bytes\n";
    }
    for (const auto& [table_name, column_ids] : column_ids_to_migrate) {
      auto& cxl_set = _cxl_columns[table_name];
      for (const auto col_id : column_ids) cxl_set.insert(col_id);
    }
    // Snapshot current per-column access totals so the first sample's deltas
    // cover benchmark accesses only (profiling pass excluded), matching the
    // semantic plugin's daemon hit-rate semantics.
    for (const auto& [table_name, table] : Hyrise::get().storage_manager.tables()) {
      const auto column_count = table->column_count();
      auto totals = std::vector<uint64_t>(column_count, 0);
      const auto chunk_count = table->chunk_count();
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        auto chunk = table->get_chunk(chunk_id);
        if (!chunk) continue;
        for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
          auto segment = chunk->get_segment(col_id);
          if (!segment) continue;
          totals[col_id] += sum_segment_accesses(segment);
        }
      }
      _prev_column_access[table_name] = std::move(totals);
    }
    _sampler_running = true;
    _sampler_thread = std::thread(&PlacementPlugin::sampler_loop, this);
    std::cout << "- [P0-3] Baseline sampler started (5s interval, csv=" << _sampler_csv_path << ")\n";
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// P0-3: baseline sampler loop — per-5s access/hit-rate sampling for b2
// ─────────────────────────────────────────────────────────────────────────────
void PlacementPlugin::sampler_loop() {
  while (_sampler_running) {
    {
      std::unique_lock<std::mutex> lock(_sampler_mutex);
      _sampler_cv.wait_for(lock, std::chrono::seconds(5), [this] { return !_sampler_running.load(); });
    }
    if (!_sampler_running) break;
    ++_sampler_cycle;

    auto local_delta = uint64_t{0};
    auto cxl_delta = uint64_t{0};
    auto& storage_manager = Hyrise::get().storage_manager;
    for (const auto& [table_name, table] : storage_manager.tables()) {
      const auto column_count = table->column_count();
      auto totals = std::vector<uint64_t>(column_count, 0);
      const auto chunk_count = table->chunk_count();
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        auto chunk = table->get_chunk(chunk_id);
        if (!chunk) continue;
        for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
          auto segment = chunk->get_segment(col_id);
          if (!segment) continue;
          totals[col_id] += sum_segment_accesses(segment);
        }
      }
      auto prev_it = _prev_column_access.find(table_name);
      if (prev_it != _prev_column_access.end() && prev_it->second.size() == column_count) {
        const auto& prev = prev_it->second;
        const auto cxl_it = _cxl_columns.find(table_name);
        for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
          const auto cur = totals[col_id];
          const auto delta = cur >= prev[col_id] ? cur - prev[col_id] : 0;
          const auto is_cxl = cxl_it != _cxl_columns.end() && cxl_it->second.count(col_id) > 0;
          if (is_cxl) {
            cxl_delta += delta;
          } else {
            local_delta += delta;
          }
        }
      }
      _prev_column_access[table_name] = std::move(totals);
    }

    const auto total_access = local_delta + cxl_delta;
    // Convention: no accesses in interval → 100.0 (vacuous; noted in MANIFEST)
    const auto hit_rate_pct =
        total_access > 0 ? 100.0 * static_cast<double>(local_delta) / total_access : 100.0;
    std::ofstream ofs(_sampler_csv_path, std::ios::app);
    ofs << time(nullptr) << "," << _sampler_cycle << "," << hit_rate_pct
        << ",0,0,0,0,0.000000,0\n";
  }
  std::cout << "- [P0-3] Baseline sampler exiting (cycles=" << _sampler_cycle << ")\n";
}

std::optional<PreBenchmarkHook> PlacementPlugin::pre_benchmark_hook() {
  return [&](auto& benchmark_item_runner) {
    const auto& options = Hyrise::get().placement_options;
    std::cout << "Options [" << options << "]" << std::endl;
    if (!options.rmem_node_ids.empty()) {
      // Honor explicit capacity if set (>0), else fall back to scale_factor × SIZE_GIB.
      const auto cxl_buf = options.cxl_capacity_gb > 0.0f
                               ? static_cast<uint64_t>(options.cxl_capacity_gb * SIZE_GIB)
                               : static_cast<uint64_t>(options.scale_factor * SIZE_GIB * 1.0);
      auto memory_resource = std::make_unique<LinearNumaMemoryResource>(cxl_buf, options.rmem_node_ids, options.rmem_weights);
      Hyrise::get().memory_resources.emplace("cxl", std::move(memory_resource));
    }

    auto& resources = Hyrise::get().memory_resources;
    if (!resources.contains("cxl")) {
      std::cout << "- Not migrating any table data as no memory resource was configured." << std::endl;
      return;
    }

    switch (options.type) {
      case PlacementType::AccessFrequency:
          run_column_placement(benchmark_item_runner);
          break;
      case PlacementType::PagesRoundRobinInterleaved:
      case PlacementType::PagesWeightedInterleaved:
          run_page_placement(benchmark_item_runner);
          break;
      case PlacementType::None:
          std::cout << "- Not migrating any table data as placement type is None." << std::endl;
          return;
    }
  };
}

std::optional<PostBenchmarkHook> PlacementPlugin::post_benchmark_hook() {
  return [&](auto& report) {
    // P0-3: stop the baseline access sampler thread
    if (_sampler_thread.joinable()) {
      {
        std::lock_guard<std::mutex> lock(_sampler_mutex);
        _sampler_running = false;
      }
      _sampler_cv.notify_all();
      _sampler_thread.join();
    }
  };
}

EXPORT_PLUGIN(PlacementPlugin);

}  // namespace hyrise
