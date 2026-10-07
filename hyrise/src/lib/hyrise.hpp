#pragma once

#include <atomic>
#include <memory>

#include "concurrency/transaction_manager.hpp"
#include "memory/linear_numa_memory_resource.hpp"
#include "scheduler/abstract_scheduler.hpp"
#include "scheduler/topology.hpp"
#include "sql/sql_plan_cache.hpp"
#include "storage/storage_manager.hpp"
#include "utils/log_manager.hpp"
#include "utils/meta_table_manager.hpp"
#include "utils/plugin_manager.hpp"
#include "utils/settings_manager.hpp"
#include "utils/singleton.hpp"
#include "types.hpp"

namespace hyrise {

class BenchmarkRunner;

// This should be the only singleton in the src/lib world. It provides a unified way of accessing components like the
// storage manager, the transaction manager, and more. Encapsulating this in one class avoids the static initialization
// order fiasco, which would otherwise make the initialization/destruction order hard to control.
class Hyrise : public Singleton<Hyrise> {
 public:
  // Resets the Hyrise state by deleting its members (e.g., StorageManager) and
  // creating new ones. This is used especially in tests and can lead to a lot of
  // issues if there are still running tasks / threads that want to access a resource.
  // You should be very sure that this is what you want.
  static void reset();

  // The scheduler is always set. However, the ImmediateExecutionScheduler does not involve any multi-threading. This
  // can be tested with is_multi_threaded.
  const std::shared_ptr<AbstractScheduler>& scheduler() const;
  bool is_multi_threaded() const;

  void set_scheduler(const std::shared_ptr<AbstractScheduler>& new_scheduler);

  void set_numa_memory_resource(const NumaNodeIDs& node_ids, const uint64_t size);

  // The order of these members is important because it defines in which order their destructors are called.
  // For example, the StorageManager's destructor should not be called before the PluginManager's destructor.
  // The latter stops all plugins which, in turn, might access tables during their shutdown procedure. This
  // could not work without the StorageManager still in place.

  // Memory resource string conventions:
  // - "cxl" for memory resources used for CXL memory allocations
  // - "local" for CPU-local memory resource
  std::unordered_map<std::string, std::unique_ptr<MemoryResource>> memory_resources;
  PlacementOptions placement_options;

  // [IMPL-1] (plan v3 §4): set to true by the BenchmarkRunner at the first instant
  // of the timed phase (Shuffled mode) when placement_options.placement_freeze_at_timed_start
  // is enabled. The SemanticPlacementPlugin daemon observes it at run time and switches
  // to pure-measurement cycles (no promote/demote/GC; hit_rate rows continue).
  // Movable wrapper: Hyrise::reset() move-assigns the singleton and std::atomic is
  // not movable; the wrapper preserves the value across such a reset.
  struct MovableAtomicBool {
    std::atomic<bool> value{false};
    MovableAtomicBool() = default;
    MovableAtomicBool(MovableAtomicBool&& other) noexcept {
      value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    MovableAtomicBool& operator=(MovableAtomicBool&& other) noexcept {
      value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }
    void store(bool desired, std::memory_order order = std::memory_order_seq_cst) {
      value.store(desired, order);
    }
    bool load(std::memory_order order = std::memory_order_seq_cst) const { return value.load(order); }
  };
  MovableAtomicBool placement_frozen;

  // EDBT v4 Phase B (commit B1): timed-phase anchors, set by the BenchmarkRunner
  // at the first instant of the Shuffled timed phase (unconditionally; two atomic
  // stores off the decision path). The SemanticPlacementPlugin daemon reads them
  // to label every decision_trace row warmup / timed_first_half / timed_second_half.
  // -1 (start) / 0 (planned duration) = warmup / unknown duration.
  // Movable wrappers for the same reason as placement_frozen above.
  template <typename T>
  struct MovableAtomic {
    std::atomic<T> value{};
    MovableAtomic() = default;
    explicit MovableAtomic(T initial) {
      value.store(initial, std::memory_order_relaxed);
    }
    MovableAtomic(MovableAtomic&& other) noexcept {
      value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    MovableAtomic& operator=(MovableAtomic&& other) noexcept {
      value.store(other.value.load(std::memory_order_relaxed), std::memory_order_relaxed);
      return *this;
    }
    void store(T desired, std::memory_order order = std::memory_order_seq_cst) { value.store(desired, order); }
    T load(std::memory_order order = std::memory_order_seq_cst) const { return value.load(order); }
  };
  MovableAtomic<int64_t> timed_phase_start_steady_ns{-1};
  MovableAtomic<uint64_t> timed_phase_planned_duration_ns{0};

  StorageManager storage_manager;
  PluginManager plugin_manager;
  TransactionManager transaction_manager;
  MetaTableManager meta_table_manager;
  SettingsManager settings_manager;
  LogManager log_manager;
  Topology topology;

  // Plan caches used by the SQLPipelineBuilder if `with_{l/p}qp_cache()` are not used. Both default caches can be
  // nullptr themselves. If both default_{l/p}qp_cache and _{l/p}qp_cache are nullptr, no plan caching is used.
  std::shared_ptr<SQLPhysicalPlanCache> default_pqp_cache;
  std::shared_ptr<SQLLogicalPlanCache> default_lqp_cache;

  // The BenchmarkRunner is available here so that non-benchmark components can add information to the benchmark
  // result JSON.
  std::weak_ptr<BenchmarkRunner> benchmark_runner;

 private:
  Hyrise();
  friend class Singleton;

  // (Re-)setting the scheduler requires more than just replacing the pointer. To make sure that set_scheduler is used,
  // the scheduler is private.
  std::shared_ptr<AbstractScheduler> _scheduler;
};

}  // namespace hyrise
