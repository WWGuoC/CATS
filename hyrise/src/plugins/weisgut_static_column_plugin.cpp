#include "weisgut_static_column_plugin.hpp"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
#include <utility>

#include "hyrise.hpp"
#include "resolve_type.hpp"
#include "storage/chunk.hpp"
#include "storage/segment_access_counter.hpp"
#include "storage/segment_iterate.hpp"
#include "storage/value_segment.hpp"

namespace hyrise {

// std::size_t of one GiB, matching the semantic plugin's capacity math
// (float32 CLI value × 2^30, truncated).
namespace {
constexpr double SIZE_GIB_D = 1073741824.0;

uint64_t budget_from_capacity_gb(float capacity_gb) {
  return capacity_gb > 0.0f ? static_cast<uint64_t>(capacity_gb * SIZE_GIB_D) : 0;
}

}  // namespace

std::string WeisgutStaticColumnPlugin::description() const {
  return "WeisgutStaticColumnPlugin";
}

void WeisgutStaticColumnPlugin::start() {}
void WeisgutStaticColumnPlugin::stop() {}

std::optional<PreBenchmarkHook> WeisgutStaticColumnPlugin::pre_benchmark_hook() {
  return [this](auto&) {
    const auto& options = Hyrise::get().placement_options;
    _static_mode = options.weisgut_static_column && !options.column_profile_path.empty();
    _profile_mode = !options.column_profile_out.empty();
    _profile_path = options.column_profile_path;
    _profile_out_path = options.column_profile_out;
    _map_start_path = options.json_path.empty() ? "placement_map_start.csv"
                                                : options.json_path.substr(0, options.json_path.size() - 5) + "_placement_map_start.csv";
    _map_end_path = _map_start_path.substr(0, _map_start_path.size() - std::string("_start.csv").size()) + "_end.csv";
    _selected_columns_path = options.json_path.empty() ? "selected_columns.csv"
                                                        : options.json_path.substr(0, options.json_path.size() - 5) + "_selected_columns.csv";
    _overhead_path = options.json_path.empty() ? "overhead.csv"
                                               : options.json_path.substr(0, options.json_path.size() - 5) + "_overhead.csv";

    std::cout << "=== WeisgutStaticColumnPlugin pre_benchmark_hook ===\n";
    if (_profile_mode) {
      std::cout << "- [WEISGUT-W] PROFILE MODE: observe-only (no placement, no daemon, "
                   "no replicas; five typed counters summed per column)\n";
      return;
    }
    if (!_static_mode) {
      std::cout << "- [WEISGUT-W] inactive (neither --column_profile_out nor "
                   "--weisgut_static_column given)\n";
      return;
    }

    _budget_bytes = budget_from_capacity_gb(options.dram_capacity_gb);
    const auto cxl_buf = budget_from_capacity_gb(options.cxl_capacity_gb);
    std::cout << "- [WEISGUT-W] STATIC MODE: budget_bytes=" << _budget_bytes
              << " (float32(" << options.dram_capacity_gb << ") GiB), CXL buffer "
              << (cxl_buf / (1024.0 * 1024 * 1024)) << " GiB\n";

    // 1. memory resources (local DRAM node1 / CXL node2) — registered in the
    // GLOBAL Hyrise::memory_resources map (semantic-plugin pattern). Plugin-
    // member ownership made the resources die at plugin unload while replaced
    // segments still referenced their memory -> exit-time segfault (gdb:
    // ~StorageManager -> ~DictionarySegment -> ~FixedWidthIntegerVector).
    auto& resources = Hyrise::get().memory_resources;
    if (options.lmem_node_ids.empty() || options.rmem_node_ids.empty()) {
      std::cout << "- [WEISGUT-W] ERROR: need --lmem_ids and --rmem_ids\n";
      return;
    }
    resources.erase("weisgut_local");
    resources.erase("weisgut_cxl");
    {
      auto local = std::make_unique<LinearNumaMemoryResource>(
          _budget_bytes, options.lmem_node_ids);
      _local_resource = local.get();
      resources.emplace("weisgut_local", std::move(local));
      auto cxl = std::make_unique<LinearNumaMemoryResource>(
          cxl_buf, options.rmem_node_ids, options.rmem_weights);
      _cxl_resource = cxl.get();
      resources.emplace("weisgut_cxl", std::move(cxl));
    }

    // 2. frozen profile input (never the current run's counters)
    _read_profile(_profile_path);

    // 3. sort: access_count desc, then table_name asc, then column_id asc
    std::sort(_columns.begin(), _columns.end(), [](const ColumnRow& a, const ColumnRow& b) {
      if (a.access_count != b.access_count) return a.access_count > b.access_count;
      if (a.table_name != b.table_name) return a.table_name < b.table_name;
      return a.column_id < b.column_id;
    });

    // 4. greedy whole-column packing (skip-and-continue, spec §5.3.4)
    _pack_columns();

    // 5. one-time placement: selected → node1, everything else → node2
    _placement_t0 = std::chrono::steady_clock::now();
    _place_columns();
    _placement_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - _placement_t0).count();

    _write_selected_columns_csv();
    _write_placement_map(_map_start_path);
    std::cout << "- [WEISGUT-W] STATIC PLACEMENT FROZEN: selected "
              << std::count_if(_columns.begin(), _columns.end(),
                               [](const auto& c) { return c.selected; })
              << "/" << _columns.size() << " columns, selected_bytes=" << _selected_bytes
              << " <= budget " << _budget_bytes << ", placement " << _placement_time_ms
              << " ms, " << _placement_bytes << " bytes copied, "
              << _migrated_segments << " segments moved, "
              << _mutable_segments_skipped << " mutable segments left in place\n"
              << "- [WEISGUT-W] timed-phase contract: NO daemon, NO replicas, "
                 "NO migration (mapping is frozen)\n";
  };
}

std::optional<PostBenchmarkHook> WeisgutStaticColumnPlugin::post_benchmark_hook() {
  return [this](auto&) {
    if (_profile_mode) {
      _dump_profile();
      return;
    }
    if (!_static_mode) return;
    _write_placement_map(_map_end_path);
    _write_overhead_csv();
    std::cout << "- [WEISGUT-W] post: placement_map_end written; timed-phase "
                 "migrations = 0 by construction (no daemon)\n";
  };
}

void WeisgutStaticColumnPlugin::_read_profile(const std::string& path) {
  // column_profile.csv schema (written by _dump_profile):
  // profile_id,workload,rank,table_name,column_id,column_name,access_count,
  // column_bytes,profile_seed,profile_sha256
  _columns.clear();
  std::ifstream in(path);
  Assert(in.is_open(), "[WEISGUT-W] cannot open column profile: " + path);
  std::string line;
  std::getline(in, line);  // header
  Assert(line.rfind("profile_id,", 0) == 0, "[WEISGUT-W] unexpected profile header: " + line);
  auto n = size_t{0};
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    auto fields = std::vector<std::string>{};
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
      fields.push_back(field);
    }
    Assert(fields.size() >= 7, "[WEISGUT-W] short profile row: " + line);
    ColumnRow row;
    row.table_name = fields[3];
    row.column_id = static_cast<ColumnID::base_type>(std::stoul(fields[4]));
    row.column_name = fields[5];
    row.access_count = std::stoull(fields[6]);
    _columns.push_back(std::move(row));
    ++n;
  }
  std::cout << "- [WEISGUT-W] frozen profile: " << path << " (" << n << " columns)\n";
}

void WeisgutStaticColumnPlugin::_pack_columns() {
  _selected_bytes = 0;
  for (auto& column : _columns) {
    // live byte size of the column in the CURRENT loaded, encoded store
    column.column_bytes = 0;
    const auto& sm = Hyrise::get().storage_manager;
    if (sm.has_table(column.table_name)) {
      const auto table = sm.get_table(column.table_name);
      const auto chunk_count = table->chunk_count();
      for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
        const auto& chunk = table->get_chunk(chunk_id);
        if (!chunk) continue;
        const auto& segment = chunk->get_segment(ColumnID{column.column_id});
        if (segment) {
          column.column_bytes +=
              segment->memory_usage(MemoryUsageCalculationMode::Full);
        }
      }
    }
    if (_selected_bytes + column.column_bytes <= _budget_bytes) {
      column.selected = true;
      _selected_bytes += column.column_bytes;
      column.cumulative_bytes = _selected_bytes;
      column.skip_reason = "";
    } else {
      column.selected = false;
      column.skip_reason = "exceeds remaining budget " +
                           std::to_string(_budget_bytes - _selected_bytes) + " < " +
                           std::to_string(column.column_bytes);
    }
  }
}

void WeisgutStaticColumnPlugin::_place_columns() {
  auto& sm = Hyrise::get().storage_manager;
  for (auto& column : _columns) {
    const auto& target_resource = column.selected ? _local_resource : _cxl_resource;
    const auto table = sm.get_table(column.table_name);
    const auto chunk_count = table->chunk_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      if (!chunk) continue;
      if (chunk->is_mutable()) {
        ++_mutable_segments_skipped;  // benchmark-loaded tables are immutable;
        _tier_map[column.table_name + ":" + std::to_string(chunk_id) + ":" +
                 std::to_string(column.column_id)] = "UNCHANGED_MUTABLE";
        continue;                     // mutable segments stay in place (disclosed)
      }
      const auto segment = chunk->get_segment(ColumnID{column.column_id});
      if (!segment) continue;
      if (target_resource->address_in_range(segment.get())) continue;  // already there
      auto new_segment = segment->copy_using_memory_resource(target_resource);
      if (!new_segment) continue;
      chunk->replace_segment(ColumnID{column.column_id}, std::move(new_segment));
      _tier_map[column.table_name + ":" + std::to_string(chunk_id) + ":" +
               std::to_string(column.column_id)] = column.selected ? "DRAM" : "CXL";
      ++_migrated_segments;
    }
  }
  // exact byte accounting from the map (not per-segment estimates)
  _placement_bytes = 0;
  for (const auto& column : _columns) {
    _placement_bytes += column.column_bytes;
  }
}

void WeisgutStaticColumnPlugin::_write_selected_columns_csv() const {
  std::ofstream out(_selected_columns_path);
  out << "run_id,workload,budget_label,rep,rank,table_name,column_id,column_name,"
         "access_count,column_bytes,cumulative_bytes,selected,skip_reason,profile_sha256\n";
  const auto& env_run = std::getenv("EDBT_W_RUN_ID");
  const auto& env_workload = std::getenv("EDBT_W_WORKLOAD");
  const auto& env_budget = std::getenv("EDBT_W_BUDGET_LABEL");
  const auto& env_rep = std::getenv("EDBT_W_REP");
  const auto& env_profile_sha = std::getenv("EDBT_W_PROFILE_SHA256");
  auto rank = size_t{0};
  for (const auto& column : _columns) {
    ++rank;
    out << (env_run ? env_run : "") << ',' << (env_workload ? env_workload : "") << ','
        << (env_budget ? env_budget : "") << ',' << (env_rep ? env_rep : "") << ','
        << rank << ',' << column.table_name << ',' << column.column_id << ','
        << column.column_name << ',' << column.access_count << ','
        << column.column_bytes << ',' << column.cumulative_bytes << ','
        << (column.selected ? "true" : "false") << ',' << column.skip_reason << ','
        << (env_profile_sha ? env_profile_sha : "") << '\n';
  }
}

void WeisgutStaticColumnPlugin::_write_placement_map(const std::string& path) const {
  std::ofstream out(path);
  Assert(out.is_open(), "[WEISGUT-W] cannot open placement map: " + path);
  out << "table_name,chunk_id,column_id,segment_bytes,tier\n";
  auto& sm = Hyrise::get().storage_manager;
  for (const auto& [table_name, table] : sm.tables()) {
    const auto chunk_count = table->chunk_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      if (!chunk) continue;
      for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
        const auto& segment = chunk->get_segment(column_id);
        if (!segment) continue;
        const auto key = table_name + ":" + std::to_string(chunk_id) + ":" +
                         std::to_string(column_id);
        const auto it = _tier_map.find(key);
        out << table_name << ',' << chunk_id << ',' << column_id << ','
            << segment->memory_usage(MemoryUsageCalculationMode::Full) << ','
            << (it == _tier_map.end() ? "UNCHANGED" : it->second) << '\n';
      }
    }
  }
}

void WeisgutStaticColumnPlugin::_write_overhead_csv() const {
  const auto& profile_time = std::getenv("EDBT_W_PROFILE_TIME_S");
  std::ofstream out(_overhead_path);
  out << "profile_time_s,initial_placement_time_ms,initial_placement_bytes\n";
  out << (profile_time ? profile_time : "") << ',' << _placement_time_ms << ','
      << _placement_bytes << '\n';
}

void WeisgutStaticColumnPlugin::_dump_profile() const {  // NOLINT(readability-convert-member-functions-to-static)
  // The profile output is the run's PRIMARY artifact: fail loudly if the
  // stream cannot open (e.g. missing parent directory) instead of silently
  // producing nothing.
  struct Agg {
    uint64_t count = 0;
    uint64_t bytes = 0;
    std::string name;
  };
  auto by_column = std::map<std::pair<std::string, ColumnID::base_type>, Agg>{};
  auto& sm = Hyrise::get().storage_manager;
  using SAC = SegmentAccessCounter::AccessType;
  for (const auto& [table_name, table] : sm.tables()) {
    const auto chunk_count = table->chunk_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      if (!chunk) continue;
      for (auto column_id = ColumnID{0}; column_id < table->column_count(); ++column_id) {
        const auto& segment = chunk->get_segment(column_id);
        if (!segment) continue;
        auto& agg = by_column[{table_name, column_id}];
        agg.name = table->column_name(column_id);
        const auto& counter = segment->access_counter;
        // spec §5.1: PURE sum of the five typed access counters
        agg.count += counter[SAC::Point].load(std::memory_order_relaxed) +
                     counter[SAC::Sequential].load(std::memory_order_relaxed) +
                     counter[SAC::Monotonic].load(std::memory_order_relaxed) +
                     counter[SAC::Random].load(std::memory_order_relaxed) +
                     counter[SAC::Dictionary].load(std::memory_order_relaxed);
        agg.bytes += segment->memory_usage(MemoryUsageCalculationMode::Full);
      }
    }
  }
  // rank: access_count desc, table asc, column asc (same order the packer uses)
  auto rows = std::vector<std::tuple<uint64_t, std::string, ColumnID::base_type, Agg>>{};
  for (const auto& [key, agg] : by_column) {
    rows.emplace_back(agg.count, key.first, key.second, agg);
  }
  std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
    if (std::get<0>(a) != std::get<0>(b)) return std::get<0>(a) > std::get<0>(b);
    if (std::get<1>(a) != std::get<1>(b)) return std::get<1>(a) < std::get<1>(b);
    return std::get<2>(a) < std::get<2>(b);
  });
  std::ofstream out(_profile_out_path);
  Assert(out.is_open(), "[WEISGUT-W] cannot open profile output: " + _profile_out_path);
  out << "profile_id,workload,rank,table_name,column_id,column_name,access_count,"
         "column_bytes,profile_seed,profile_sha256\n";
  const auto& env_id = std::getenv("EDBT_W_PROFILE_ID");
  const auto& env_workload = std::getenv("EDBT_W_WORKLOAD");
  const auto& env_seed = std::getenv("EDBT_W_PROFILE_SEED");
  auto rank = size_t{0};
  for (const auto& [count, table_name, column_id, agg] : rows) {
    ++rank;
    out << (env_id ? env_id : "") << ',' << (env_workload ? env_workload : "") << ','
        << rank << ',' << table_name << ',' << column_id << ',' << agg.name << ','
        << agg.count << ',' << agg.bytes << ',' << (env_seed ? env_seed : "")
        << ",SELF\n";
  }
  std::cout << "- [WEISGUT-W] PROFILE WRITTEN: " << _profile_out_path << " ("
            << rows.size() << " columns; profile_sha256 of the FILE is recorded by "
               "the runner — the SELF column marks self-reference)\n";
}

EXPORT_PLUGIN(WeisgutStaticColumnPlugin);

}  // namespace hyrise
