#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "strong_typedef.hpp"
// TODO(MW) clean include of NumaNodeIDs and InterleavingWeights
#include "memory/linear_numa_memory_resource.hpp"

/**
 * We use STRONG_TYPEDEF to avoid things like adding chunk ids and value ids. Because implicit constructors are
 * deleted, you cannot initialize a ChunkID like this:
 *   ChunkID x = 3;
 * but need to use
 *   auto x = ChunkID{3};
 * In some cases (e.g., when narrowing data types), casting to the base_type first might be necessary, e.g.:
 *   ChunkID{static_cast<ChunkID::base_type>(size_t{17})}
 *
 * We prefer strong typedefs whenever they are applicable and make sense. However, there are cases where we cannot
 * directly use them. For example, in std::atomics when we want to use C++'s specializations for integral types (e.g.,
 * `++task_id` with `std::atomic<TaskID> task_id`). Therefore, we use the base_type in atomics (e.g.,
 * `std::atomic<TaskID::base_type> task_id`).
 */

STRONG_TYPEDEF(uint32_t, ChunkID);
STRONG_TYPEDEF(uint16_t, ColumnID);
STRONG_TYPEDEF(hyrise::ColumnID::base_type, ColumnCount);
STRONG_TYPEDEF(uint32_t, ValueID);  // Cannot be larger than ChunkOffset
STRONG_TYPEDEF(uint32_t, NodeID);
STRONG_TYPEDEF(uint32_t, CpuID);
STRONG_TYPEDEF(uint32_t, WorkerID);
STRONG_TYPEDEF(uint32_t, TaskID);
STRONG_TYPEDEF(uint32_t, ChunkOffset);

// When changing the following two strong typedefs to 64-bit types, please be aware that both are used with
// std::atomics and not all platforms that Hyrise runs on support atomic 64-bit instructions. Any Intel and AMD CPU
// since 2010 should work fine. For 64-bit atomics on ARM CPUs, the instruction set should be at least ARMv8.1-A.
// Earlier instruction sets also work, but might yield less efficient code. More information can be found here:
// https://community.arm.com/arm-community-blogs/b/tools-software-ides-blog/posts/making-the-most-of-the-arm-architecture-in-gcc-10  // NOLINT
STRONG_TYPEDEF(uint32_t, CommitID);
STRONG_TYPEDEF(uint32_t, TransactionID);

// Used to identify a Parameter within a subquery. This can be either a parameter of a Prepared SELECT statement
// `SELECT * FROM t WHERE a > ?` or a correlated parameter in a subquery.
STRONG_TYPEDEF(uint16_t, ParameterID);

namespace hyrise {

// Float aliases used in cardinality estimations/statistics
using Cardinality = float;
using DistinctCount = float;
using Selectivity = float;

// Cost that an AbstractCostModel assigns to an Operator/LQP node. The unit of the Cost is left to the Cost estimator
// and could be, e.g., "Estimated Runtime" or "Estimated Memory Usage" (though the former is by far the most common)
using Cost = float;

// We use polymorphic memory resources to allow containers (e.g., vectors, or strings) to retrieve their memory from
// different memory sources. These sources are, for example, specific NUMA nodes or non-volatile memory. Without PMR,
// we would need to explicitly make the allocator part of the class. This would make DRAM and NVM containers type-
// incompatible. Thanks to PMR, the type is erased and both can co-exist.
//
template <typename T>
using PolymorphicAllocator = std::pmr::polymorphic_allocator<T>;
using MemoryResource = std::pmr::memory_resource;

enum class PlacementType { None, PagesRoundRobinInterleaved, PagesWeightedInterleaved, AccessFrequency, SemanticProactive };

struct PlacementOptions {
  float scale_factor;
  NumaNodeIDs lmem_node_ids;
  NumaNodeIDs rmem_node_ids;
  InterleavingWeights rmem_weights;
  PlacementType type;
  uint32_t num_most_frequently_columns_local;
  std::string json_path;
  // Ablation flags for semantic placement (Δ1/Δ2/Δ3). Default false = full pipeline.
  bool ablate_d1 = false;           // true → use access-frequency instead of LQP plan analysis
  bool ablate_d2 = false;           // true → column-level migration instead of segment-level
  bool ablate_d3_replica = false;   // true → single-copy migration, no replicas
  bool ablate_d3_gc = false;        // true → no semantic GC, LRU fallback only
  // DRAM-first mode + periodic migration daemon configuration
  bool dram_first = false;               // true → data starts on DRAM, demote cold segments to CXL
  uint32_t daemon_period_seconds = 5;    // daemon wake-up period N (seconds); 0 = no daemon
  float demote_pct = 0.3f;               // fraction of cold segments to demote to CXL in pre-placement (0–1)
  float dram_capacity_gb = 0.0f;         // 0 = auto (scale_factor × SIZE_GIB); >0 = explicit DRAM buffer size in GB
  float cxl_capacity_gb = 0.0f;          // 0 = auto (scale_factor × SIZE_GIB); >0 = explicit CXL buffer size in GB
  // [IMPL-1] (plan v3 §4): freeze placement at timed-phase start. Warmup-phase
  // placement (dram_first mechanism, budget-clamped) runs unchanged; from the
  // first instant of the timed phase the semantic daemon stops promoting/demoting
  // but keeps sampling (hit_rate rows continue, migration columns all zero).
  bool placement_freeze_at_timed_start = false;
  // [IMPL-2] (plan v3 §4, formula A human-specified 2026-08-31): cost-weighted
  // frequency priority for the E8 arm. freq_w(s) = Σ_a cost(atype(a)) over accesses
  // to segment s; with the per-segment tracked type this equals cost(atype(s)) ×
  // access_rate(s). Normalization by this round's max freq_w. P(s) keeps the
  // frequency-arm shape (atype term neutralized to the Unknown=0.5 constant);
  // semantic and frequency arms' code paths are untouched.
  bool placement_wfreq = false;
  // [IMPL-4] (plan v3 §4, E9B): cardinality-estimate perturbation kappa.
  // Multiplied into every optimizer cardinality estimate read by the Plan
  // Analyzer (single injection boundary). 1.0 = identity.
  float placement_kappa = 1.0f;
  // [IMPL-5] (plan v3 §4, E10): directory the Plan Analyzer reads JOB-style
  // .sql files from. Empty = default third_party path (wave-1 behavior);
  // set to the benchmark's actual --query_path so plan analysis follows the
  // workload being run (synthetic mixed-workload driver).
  std::string placement_query_path;
  // EDBT v4 Phase B (commit B1): observation-only placement trace, default OFF.
  // When false, every code path added for the trace is inert (trace-off decision
  // equivalence vs the base commit is proven before any mapper change lands).
  // When true, the SemanticPlacementPlugin writes <json>_decision_trace.csv
  // (one row per considered segment per daemon cycle) and
  // <json>_operator_segment_trace.csv (plan_reference_only rows) next to the
  // benchmark JSON output. Run identity for the CSV rows comes from the
  // EDBT_V4_RUN_ID / EDBT_V4_REP environment variables (harness-controlled),
  // defaulting to the JSON file stem and rep 1.
  bool placement_trace = false;
  // EDBT v4 Phase B2: single-client MEASURED diagnostic pass (default OFF).
  // Requires placement_trace and the JOB CLI only allows it with --clients 1.
  // After the initial placement and before the migration daemon starts, the
  // plugin executes each workload query once, sequentially, taking segment
  // counter snapshots before/after every query: the per-segment five-pattern
  // counter delta is a true measured value for that execution window
  // (attribution_level=measured_per_execution; written to
  // <json>_measured_segment_access.csv). Per-operator wall/rows come from the
  // executed PQPs' performance data (<json>_operator_segment_trace.csv gains
  // measured_per_execution rows); actual_reads on an operator row is filled
  // ONLY when exactly one PQP scan of that query references the segment's
  // (table, column) — otherwise it stays null (never fabricated). The main
  // clients=10 mechanism runs never enable this path and stay
  // plan_reference_only / counter-free.
  bool placement_trace_measured = false;
  // EDBT v4 Phase B (commit B3): cost_c3_greedy baseline priority provider.
  // cost(s, dev) = Σ_γ calibration(dev, γ, dtype(s)) × count_γ(s) ×
  // bytes_per_value(s) over the four-pattern counter set
  // {sequential, monotonic, random, point} of the segment access counter
  // (raw cumulative values, no EMA; Dictionary accesses are counted but
  // excluded from the cost model and reported). benefit(s) = cost(s,CXL) −
  // cost(s,DRAM) feeds the SHARED strict-fit mapper (commit B2) — only the
  // priority provider differs from our_full/our_freq, never the mapper, byte
  // budget, copy path, or measurement window. Mutually exclusive with
  // ablate_d1 and placement_wfreq (enforced by the JOB CLI).
  bool cost_c3_greedy = false;
  // Calibration CSV path with columns
  //   device,gamma,data_type,bytes_per_value,ns_per_access_per_byte
  // device ∈ {dram, cxl}; gamma ∈ {sequential, monotonic, random, point};
  // data_type ∈ {int32, int64, float32, float64, string16}. Required when
  // cost_c3_greedy is set; its SHA-256 is printed in the BASELINE startup
  // line and recorded by the run harness.
  std::string c3_calibration_path;

  // ── EDBT v4 Phase W: Weisgut-style static whole-column placement baseline ──
  // All fields additive with neutral defaults: no existing policy (including
  // our_full/our_freq/cost_c3_greedy/static_* ablation arms) reads them.
  // weisgut_static_column: enable the one-time whole-column packing + frozen
  // placement (no daemon, no replicas, no semantic planning). Requires
  // column_profile_path (a FROZEN column_profile.csv from an independent
  // profiling run — never the current run's counters).
  bool weisgut_static_column = false;
  // Frozen profile input for static mode.
  std::string column_profile_path;
  // Profile mode: observe-only run; post_benchmark_hook dumps column_profile.csv
  // (pure five-typed-counter sum per column). Mutually exclusive with
  // weisgut_static_column by construction.
  std::string column_profile_out;
};

// The string type that is used internally to store data. It's hard to draw the line between this and std::string or
// give advice when to use what. Generally, everything that is user-supplied data (mostly, data stored in a table) is a
// pmr_string. Also, the string literals in SQL queries will get converted into a pmr_string (and then stored in an
// AllTypeVariant). This way, they can be compared to the pmr_string stored in the table. Strings that are built, e.g.,
// for debugging, do not need to use PMR. This might sound complicated, but since the Hyrise data type registered in
// all_type_variant.hpp is pmr_string, the compiler will complain if you use std::string when you should use pmr_string.
using pmr_string = std::basic_string<char, std::char_traits<char>, PolymorphicAllocator<char>>;

// A vector that gets its memory from a memory resource. It is is not necessary to replace each and every std::vector
// with this. It only makes sense to use this if you also supply a memory resource. Otherwise, default memory will be
// used and we do not gain anything but have minimal runtime overhead. As a side note, PMR propagates, so a
// `pmr_vector<pmr_string>` will pass its memory resource down to the strings while a `pmr_vector<std::string>` will
// allocate the space for the vector at the correct location while the content of the strings will be in default
// storage.
// Note that a container initialized with a given allocator will keep that allocator, even if it is copy/move assigned:
//   pmr_vector<int> a, b{alloc};
//   a = b;  // a does NOT use alloc, neither for its current values, nor for future allocations (#623).
template <typename T>
using pmr_vector = std::vector<T, PolymorphicAllocator<T>>;

constexpr ChunkOffset INVALID_CHUNK_OFFSET{std::numeric_limits<ChunkOffset::base_type>::max()};
constexpr ChunkID INVALID_CHUNK_ID{std::numeric_limits<ChunkID::base_type>::max()};

struct RowID {
  constexpr RowID(const ChunkID init_chunk_id, const ChunkOffset init_chunk_offset)
      : chunk_id{init_chunk_id}, chunk_offset{init_chunk_offset} {}

  RowID() = default;

  ChunkID chunk_id{INVALID_CHUNK_ID};
  ChunkOffset chunk_offset{INVALID_CHUNK_OFFSET};

  // Faster than row_id == NULL_ROW_ID, since we only compare the ChunkOffset.
  bool is_null() const {
    return chunk_offset == INVALID_CHUNK_OFFSET;
  }

  // Joins need to use RowIDs as keys for maps.
  bool operator<(const RowID& other) const {
    return std::tie(chunk_id, chunk_offset) < std::tie(other.chunk_id, other.chunk_offset);
  }

  // Useful when comparing a row ID to NULL_ROW_ID
  bool operator==(const RowID& other) const {
    return std::tie(chunk_id, chunk_offset) == std::tie(other.chunk_id, other.chunk_offset);
  }

  friend std::ostream& operator<<(std::ostream& stream, const RowID& row_id) {
    stream << "RowID(" << row_id.chunk_id << "," << row_id.chunk_offset << ")";
    return stream;
  }
};

using CompressedVectorTypeID = uint8_t;

using ColumnIDPair = std::pair<ColumnID, ColumnID>;

constexpr NodeID INVALID_NODE_ID{std::numeric_limits<NodeID::base_type>::max()};
constexpr TaskID INVALID_TASK_ID{std::numeric_limits<TaskID::base_type>::max()};
constexpr CpuID INVALID_CPU_ID{std::numeric_limits<CpuID::base_type>::max()};
constexpr WorkerID INVALID_WORKER_ID{std::numeric_limits<WorkerID::base_type>::max()};
constexpr ColumnID INVALID_COLUMN_ID{std::numeric_limits<ColumnID::base_type>::max()};

// TransactionID = 0 means "not set" in the MVCC data. This is the case if the row has (a) just been reserved, but not
// yet filled with content, (b) been inserted, committed and not marked for deletion, or (c) inserted but deleted in
// the same transaction (which has not yet committed)
constexpr auto INVALID_TRANSACTION_ID = TransactionID{0};
constexpr auto INITIAL_TRANSACTION_ID = TransactionID{1};

constexpr NodeID CURRENT_NODE_ID{std::numeric_limits<NodeID::base_type>::max() - 1};

// Declaring one part of a RowID as invalid would suffice to represent NULL values. However, this way we add an extra
// safety net which ensures that NULL values are handled correctly. E.g., getting a chunk with INVALID_CHUNK_ID
// immediately crashes.
constexpr RowID NULL_ROW_ID = RowID{INVALID_CHUNK_ID, INVALID_CHUNK_OFFSET};

constexpr ValueID INVALID_VALUE_ID{std::numeric_limits<ValueID::base_type>::max()};

// The Scheduler currently supports just these two priorities.
enum class SchedulePriority {
  Default = 1,  // Schedule task of normal priority.
  High = 0      // Schedule task of high priority, subject to be preferred in scheduling.
};

enum class PredicateCondition {
  Equals,
  NotEquals,
  LessThan,
  LessThanEquals,
  GreaterThan,
  GreaterThanEquals,
  BetweenInclusive,
  BetweenLowerExclusive,
  BetweenUpperExclusive,
  BetweenExclusive,
  In,
  NotIn,
  Like,
  NotLike,
  IsNull,
  IsNotNull
};

// @return whether the PredicateCondition takes exactly two arguments
bool is_binary_predicate_condition(PredicateCondition predicate_condition);

// @return whether the PredicateCondition takes exactly two arguments and is not one of LIKE or IN
bool is_binary_numeric_predicate_condition(PredicateCondition predicate_condition);

bool is_between_predicate_condition(PredicateCondition predicate_condition);

bool is_lower_inclusive_between(PredicateCondition predicate_condition);

bool is_upper_inclusive_between(PredicateCondition predicate_condition);

// ">" becomes "<" etc.
PredicateCondition flip_predicate_condition(PredicateCondition predicate_condition);

// ">" becomes "<=" etc.
PredicateCondition inverse_predicate_condition(PredicateCondition predicate_condition);

// Split up, e.g., BetweenUpperExclusive into {GreaterThanEquals, LessThan}
std::pair<PredicateCondition, PredicateCondition> between_to_conditions(PredicateCondition predicate_condition);

// Join, e.g., {GreaterThanEquals, LessThan} into BetweenUpperExclusive
PredicateCondition conditions_to_between(PredicateCondition lower, PredicateCondition upper);

// Let R and S be two tables and we want to perform `R <JoinMode> S ON <condition>`
// AntiNullAsTrue:    If for a tuple Ri in R, there is a tuple Sj in S so that <condition> is NULL or TRUE, Ri is
//                      dropped. This behavior mirrors NOT IN.
// AntiNullAsFalse:   If for a tuple Ri in R, there is a tuple Sj in S so that <condition> is TRUE, Ri is
//                      dropped. This behavior mirrors NOT EXISTS
enum class JoinMode { Inner, Left, Right, FullOuter, Cross, Semi, AntiNullAsTrue, AntiNullAsFalse };

bool is_semi_or_anti_join(JoinMode join_mode);

// SQL set operations come in two flavors, with and without `ALL`, e.g., `UNION` and `UNION ALL`.
// We have a third mode (Positions) that is used to intersect position lists that point to the same table,
// see union_positions.hpp for details.
enum class SetOperationMode { Unique, All, Positions };

// According to the SQL standard, the position of NULLs is implementation-defined. In Hyrise, NULLs come before all
// values, both for ascending and descending sorts. See sort.cpp for details.
enum class SortMode { Ascending, Descending };

enum class TableType { References, Data };

enum class DescriptionMode { SingleLine, MultiLine };

enum class UseMvcc : bool { Yes = true, No = false };

enum class RollbackReason : bool { User, Conflict };

enum class MemoryUsageCalculationMode { Sampled, Full };

enum class EraseReferencedSegmentType : bool { Yes = true, No = false };

enum class MetaTableChangeType { Insert, Delete, Update };

enum class AutoCommit : bool { Yes = true, No = false };

enum class DatetimeComponent { Year, Month, Day, Hour, Minute, Second };

// Used as a template parameter that is passed whenever we conditionally erase the type of a template. This is done to
// reduce the compile time at the cost of the runtime performance. Examples are iterators, which are replaced by
// AnySegmentIterators that use virtual method calls.
enum class EraseTypes { OnlyInDebugBuild, Always };

// Defines in which order a certain column should be or is sorted.
struct SortColumnDefinition final {
  explicit SortColumnDefinition(ColumnID init_column, SortMode init_sort_mode = SortMode::Ascending)
      : column(init_column), sort_mode(init_sort_mode) {}

  ColumnID column;
  SortMode sort_mode;
};

inline bool operator==(const SortColumnDefinition& lhs, const SortColumnDefinition& rhs) {
  return lhs.column == rhs.column && lhs.sort_mode == rhs.sort_mode;
}

class Noncopyable {
 public:
  Noncopyable(const Noncopyable&) = delete;
  const Noncopyable& operator=(const Noncopyable&) = delete;

 protected:
  Noncopyable() = default;
  Noncopyable(Noncopyable&&) noexcept = default;
  Noncopyable& operator=(Noncopyable&&) noexcept = default;
  ~Noncopyable() = default;
};

// Dummy type, can be used to overload functions with a variant accepting a Null value
struct Null {};

std::ostream& operator<<(std::ostream& stream, PredicateCondition predicate_condition);
std::ostream& operator<<(std::ostream& stream, SortMode sort_mode);
std::ostream& operator<<(std::ostream& stream, JoinMode join_mode);
std::ostream& operator<<(std::ostream& stream, SetOperationMode set_operation_mode);
std::ostream& operator<<(std::ostream& stream, TableType table_type);
std::ostream& operator<<(std::ostream& stream, PlacementOptions options);

using BoolAsByteType = uint8_t;

}  // namespace hyrise

#if !defined(__GLIBCXX__)  // libstdc++ (GCC >=11) already provides std::hash<pmr::string> in C++17; define only for STLs lacking it (e.g. libc++)
namespace std {
// The hash method for pmr_string (see above). We explicitly don't use the alias here as this allows us to write
// `using pmr_string = std::string` above. If we had `pmr_string` here, we would try to redefine an existing hash
// function.
template <>
struct hash<std::basic_string<char, std::char_traits<char>, hyrise::PolymorphicAllocator<char>>> {
  size_t operator()(
      const std::basic_string<char, std::char_traits<char>, hyrise::PolymorphicAllocator<char>>& string) const {
    return std::hash<std::string_view>{}(string);
  }
};
}  // namespace std
#endif
