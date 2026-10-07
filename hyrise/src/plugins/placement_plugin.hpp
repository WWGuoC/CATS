#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "hyrise.hpp"
#include "utils/abstract_plugin.hpp"

namespace hyrise {

class PlacementPlugin : public AbstractPlugin {
 public:
  PlacementPlugin() {}

  std::string description() const final;

  void start() final;

  void stop() final;

  std::vector<std::pair<PluginFunctionName, PluginFunctionPointer>> provided_user_executable_functions() final;

  void run_page_placement(AbstractBenchmarkItemRunner& benchmark_item_runner);

  void run_column_placement(AbstractBenchmarkItemRunner& benchmark_item_runner);

  std::optional<PreBenchmarkHook> pre_benchmark_hook() final;

  std::optional<PostBenchmarkHook> post_benchmark_hook() final;

 private:
  // ── P0-3 (plan v2 §4): baseline access sampler ────────────────────────────
  // b2 has no migration daemon; this background thread samples access counters
  // every 5 s and appends rows isomorphic to the semantic plugin's
  // daemon_metrics.csv (migration columns = 0), so E4 curves are comparable.
  void sampler_loop();
  std::thread _sampler_thread;
  std::atomic<bool> _sampler_running{false};
  std::condition_variable _sampler_cv;
  std::mutex _sampler_mutex;
  std::string _sampler_csv_path;
  // table → set of CXL-resident column ids (all other columns are DRAM/local)
  std::unordered_map<std::string, std::unordered_set<ColumnID::base_type>> _cxl_columns;
  // table → last per-column access totals (for inter-sample deltas)
  std::unordered_map<std::string, std::vector<uint64_t>> _prev_column_access;
  uint64_t _sampler_cycle = 0;
};

}  // namespace hyrise
