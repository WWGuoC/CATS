#include "plan_analyzer.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>

#include <boost/algorithm/string/replace.hpp>

#include "expression/abstract_expression.hpp"
#include "expression/binary_predicate_expression.hpp"
#include "expression/lqp_column_expression.hpp"
#include "hyrise.hpp"
#include "logical_query_plan/aggregate_node.hpp"
#include "logical_query_plan/join_node.hpp"
#include "logical_query_plan/lqp_utils.hpp"
#include "logical_query_plan/predicate_node.hpp"
#include "logical_query_plan/sort_node.hpp"
#include "logical_query_plan/stored_table_node.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "statistics/table_statistics.hpp"
#include "storage/chunk.hpp"
#include "storage/table.hpp"
// TPC-H query SQL strings (duplicated from tpch_queries.cpp to avoid linking
// hyriseBenchmarkLib, which would cause duplicate static symbols with the main
// binary and crash on exit).
static const std::map<size_t, const char*> tpch_query_strings = {
    {1, R"(SELECT l_returnflag, l_linestatus, SUM(l_quantity) as sum_qty, SUM(l_extendedprice) as sum_base_price,
      SUM(l_extendedprice*(1-l_discount)) as sum_disc_price,
      SUM(l_extendedprice*(1-l_discount)*(1+l_tax)) as sum_charge, AVG(l_quantity) as avg_qty,
      AVG(l_extendedprice) as avg_price, AVG(l_discount) as avg_disc, COUNT(*) as count_order
      FROM lineitem
      WHERE l_shipdate <= ?
      GROUP BY l_returnflag, l_linestatus
      ORDER BY l_returnflag, l_linestatus;)"},
    {2, R"(SELECT s_acctbal, s_name, n_name, p_partkey, p_mfgr, s_address, s_phone, s_comment
       FROM part, supplier, partsupp, nation, region
       WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND p_size = ? AND p_type like ? AND
       s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = ? AND
       ps_supplycost = (SELECT min(ps_supplycost) FROM partsupp, supplier, nation, region
       WHERE p_partkey = ps_partkey AND s_suppkey = ps_suppkey AND s_nationkey = n_nationkey
       AND n_regionkey = r_regionkey AND r_name = ?) ORDER BY s_acctbal DESC, n_name, s_name, p_partkey LIMIT 100;)"},
    {3, R"(SELECT l_orderkey, SUM(l_extendedprice*(1-l_discount)) as revenue, o_orderdate, o_shippriority
      FROM customer, orders, lineitem
      WHERE c_mktsegment = ? AND c_custkey = o_custkey AND l_orderkey = o_orderkey
      AND o_orderdate < ? AND l_shipdate > ?
      GROUP BY l_orderkey, o_orderdate, o_shippriority
      ORDER BY revenue DESC, o_orderdate
      LIMIT 10;)"},
    {4, R"(SELECT o_orderpriority, count(*) as order_count FROM orders WHERE o_orderdate >= ? AND
    o_orderdate < ? AND exists (
    SELECT * FROM lineitem WHERE l_orderkey = o_orderkey AND l_commitdate < l_receiptdate)
    GROUP BY o_orderpriority ORDER BY o_orderpriority;)"},
    {5, R"(SELECT n_name, SUM(l_extendedprice * (1 - l_discount)) as revenue
      FROM customer, orders, lineitem, supplier, nation, region
      WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND l_suppkey = s_suppkey AND c_nationkey = s_nationkey
      AND s_nationkey = n_nationkey AND n_regionkey = r_regionkey AND r_name = ? AND o_orderdate >= ?
      AND o_orderdate < ?
      GROUP BY n_name
      ORDER BY revenue DESC;)"},
    {6, R"(SELECT sum(l_extendedprice*l_discount) AS revenue
      FROM lineitem
      WHERE l_shipdate >= ? AND l_shipdate < ?
      AND l_discount BETWEEN ? - 0.01 AND ? + 0.01001 AND l_quantity < ?;)"},
    {7, R"(SELECT
          supp_nation,
          cust_nation,
          l_year,
          SUM(volume) as revenue
      FROM
          (SELECT
              n1.n_name as supp_nation,
              n2.n_name as cust_nation,
              SUBSTR(l_shipdate, 1, 4) as l_year,
              l_extendedprice * (1 - l_discount) as volume
          FROM
              supplier,
              lineitem,
              orders,
              customer,
              nation n1,
              nation n2
          WHERE
              s_suppkey = l_suppkey AND
              o_orderkey = l_orderkey AND
              c_custkey = o_custkey AND
              s_nationkey = n1.n_nationkey AND
              c_nationkey = n2.n_nationkey AND
              ((n1.n_name = ? AND n2.n_name = ?) OR
               (n1.n_name = ? AND n2.n_name = ?)) AND
              l_shipdate BETWEEN ? AND ?
          ) as shipping
      GROUP BY
          supp_nation, cust_nation, l_year
      ORDER BY
          supp_nation, cust_nation, l_year;)"},
    {8, R"(SELECT o_year, SUM(case when nation = ? then volume else 0 end) / SUM(volume) as mkt_share
     FROM (SELECT SUBSTR(o_orderdate, 1, 4) as o_year, l_extendedprice * (1-l_discount) as volume,
     n2.n_name as nation FROM part, supplier, lineitem, orders, customer, nation n1, nation n2, region
     WHERE p_partkey = l_partkey AND s_suppkey = l_suppkey AND l_orderkey = o_orderkey AND
     o_custkey = c_custkey AND c_nationkey = n1.n_nationkey AND n1.n_regionkey = r_regionkey AND
     r_name = ? AND s_nationkey = n2.n_nationkey AND o_orderdate between ?
     AND ? AND p_type = ?) as all_nations GROUP BY o_year ORDER BY o_year;)"},
    {9, R"(SELECT nation, o_year, SUM(amount) as sum_profit FROM (SELECT n_name as nation, SUBSTR(o_orderdate, 1, 4) as o_year,
      l_extendedprice * (1 - l_discount) - ps_supplycost * l_quantity as amount
      FROM part, supplier, lineitem, partsupp, orders, nation WHERE s_suppkey = l_suppkey
      AND ps_suppkey = l_suppkey AND ps_partkey = l_partkey AND p_partkey = l_partkey AND o_orderkey = l_orderkey
      AND s_nationkey = n_nationkey AND p_name like ?) as profit
      GROUP BY nation, o_year ORDER BY nation, o_year DESC;)"},
    {10, R"(SELECT c_custkey, c_name, SUM(l_extendedprice * (1 - l_discount)) as revenue, c_acctbal, n_name, c_address,
      c_phone, c_comment
      FROM customer, orders, lineitem, nation
      WHERE c_custkey = o_custkey AND l_orderkey = o_orderkey AND o_orderdate >= ?
      AND o_orderdate < ? AND l_returnflag = 'R' AND c_nationkey = n_nationkey
      GROUP BY c_custkey, c_name, c_acctbal, c_phone, n_name, c_address, c_comment
      ORDER BY revenue DESC LIMIT 20;)"},
    {11, R"(SELECT ps_partkey, SUM(ps_supplycost * ps_availqty) as value FROM partsupp, supplier, nation
      WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = ?
      GROUP BY ps_partkey having SUM(ps_supplycost * ps_availqty) > (
      SELECT SUM(ps_supplycost * ps_availqty) * ? FROM partsupp, supplier, nation
      WHERE ps_suppkey = s_suppkey AND s_nationkey = n_nationkey AND n_name = ?) ORDER BY value DESC;)"},
    {12, R"(SELECT l_shipmode, SUM(case when o_orderpriority ='1-URGENT' or o_orderpriority ='2-HIGH' then 1 else 0 end)
      as high_line_count, SUM(case when o_orderpriority <> '1-URGENT' AND
      o_orderpriority <> '2-HIGH' then 1 else 0 end) as low_line_count FROM orders, lineitem
      WHERE o_orderkey = l_orderkey AND l_shipmode IN (?, ?) AND l_commitdate < l_receiptdate
      AND l_shipdate < l_commitdate AND l_receiptdate >= ? AND
      l_receiptdate < ? GROUP BY l_shipmode ORDER BY l_shipmode;)"},
    {13, R"(SELECT c_count, count(*) as custdist FROM (SELECT c_custkey, count(o_orderkey) AS c_count
      FROM customer left outer join orders on c_custkey = o_custkey AND o_comment not like ?
      GROUP BY c_custkey) as c_orders GROUP BY c_count ORDER BY custdist DESC, c_count DESC;)"},
    {14, R"(SELECT 100.00 * SUM(case when p_type like 'PROMO%' then l_extendedprice*(1-l_discount) else 0 end)
      / SUM(l_extendedprice * (1 - l_discount)) as promo_revenue FROM lineitem, part WHERE l_partkey = p_partkey
      AND l_shipdate >= ? AND l_shipdate < ?;)"},
    {16, R"(SELECT p_brand, p_type, p_size, count(distinct ps_suppkey) as supplier_cnt
      FROM partsupp, part WHERE p_partkey = ps_partkey AND p_brand <> ?
      AND p_type not like ? AND p_size in (?, ?, ?, ?, ?, ?, ?, ?)
      AND ps_suppkey not in (SELECT s_suppkey FROM supplier WHERE s_comment like '%Customer%Complaints%')
      GROUP BY p_brand, p_type, p_size ORDER BY supplier_cnt DESC, p_brand, p_type, p_size;)"},
    {17, R"(SELECT SUM(l_extendedprice) / 7.0 as avg_yearly FROM lineitem, part WHERE p_partkey = l_partkey
      AND p_brand = ? AND p_container = ? AND l_quantity < (SELECT 0.2 * avg(l_quantity)
      FROM lineitem WHERE l_partkey = p_partkey);)"},
    {18, R"(SELECT c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice, SUM(l_quantity)
      FROM customer, orders, lineitem WHERE o_orderkey in (SELECT l_orderkey FROM lineitem
      GROUP BY l_orderkey having SUM(l_quantity) > ?) AND c_custkey = o_custkey AND o_orderkey = l_orderkey
      GROUP BY c_name, c_custkey, o_orderkey, o_orderdate, o_totalprice ORDER BY o_totalprice DESC, o_orderdate
      LIMIT 100;)"},
    {19, R"(SELECT SUM(l_extendedprice * (1 - l_discount) ) as revenue FROM lineitem, part WHERE (( p_partkey = l_partkey AND
      p_brand = ? AND p_container in ( 'SM CASE', 'SM BOX', 'SM PACK', 'SM PKG') AND l_quantity >= ? AND l_quantity
      <= ? + 10 AND p_size between 1 AND 5 AND l_shipmode in ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON') or
      (p_partkey = l_partkey AND p_brand = ? AND p_container in ('MED BAG', 'MED BOX', 'MED PKG', 'MED PACK') AND
      l_quantity >= ? AND l_quantity <= ? + 10 AND p_size between 1 AND 10 AND l_shipmode in ('AIR', 'AIR REG') AND
      l_shipinstruct = 'DELIVER IN PERSON') or (p_partkey = l_partkey AND p_brand = ? AND p_container in (
      'LG CASE', 'LG BOX', 'LG PACK', 'LG PKG') AND l_quantity >= ? AND l_quantity <= ? + 10 AND p_size between 1 AND 15
      AND l_shipmode in ('AIR', 'AIR REG') AND l_shipinstruct = 'DELIVER IN PERSON'));)"},
    {20, R"(SELECT s_name, s_address FROM supplier, nation WHERE s_suppkey in (SELECT ps_suppkey FROM partsupp
      WHERE ps_partkey in (SELECT p_partkey FROM part WHERE p_name like ?) AND ps_availqty >
      (SELECT 0.5 * SUM(l_quantity) FROM lineitem WHERE l_partkey = ps_partkey AND l_suppkey = ps_suppkey AND
      l_shipdate >= ? AND l_shipdate < ?)) AND s_nationkey = n_nationkey
      AND n_name = ? ORDER BY s_name;)"},
    {21, R"(SELECT s_name, count(*) as numwait FROM supplier, lineitem l1, orders, nation WHERE s_suppkey = l1.l_suppkey
      AND o_orderkey = l1.l_orderkey AND o_orderstatus = 'F' AND l1.l_receiptdate > l1.l_commitdate AND exists
      (SELECT * FROM lineitem l2 WHERE l2.l_orderkey = l1.l_orderkey AND l2.l_suppkey <> l1.l_suppkey) AND not exists
      (SELECT * FROM lineitem l3 WHERE l3.l_orderkey = l1.l_orderkey AND l3.l_suppkey <> l1.l_suppkey AND
      l3.l_receiptdate > l3.l_commitdate ) AND s_nationkey = n_nationkey AND n_name = ? GROUP BY s_name
      ORDER BY numwait DESC, s_name LIMIT 100;)"},
    {22, R"(SELECT
         cntrycode, COUNT(*) AS numcust, SUM(c_acctbal) AS totacctbal
       FROM
         (SELECT
            SUBSTR(c_phone,1,2) AS cntrycode, c_acctbal
          FROM
            customer
          WHERE
            SUBSTR(c_phone,1,2) IN (?, ?, ?, ?, ?, ?, ?) AND
            c_acctbal > (SELECT
                           AVG(c_acctbal)
                         FROM
                           customer
                         WHERE
                          c_acctbal > 0.00 AND
                          SUBSTR(c_phone,1,2) IN (?, ?, ?, ?, ?, ?, ?)) AND
            NOT EXISTS ( SELECT * FROM orders WHERE o_custkey = c_custkey)
        ) AS custsale
       GROUP BY cntrycode
       ORDER BY cntrycode;)"}
};

namespace hyrise {

// Deterministic parameter values for TPC-H queries 1-22 (copied from
// TPCHBenchmarkItemRunner::_build_deterministic_query). Query 15 is handled
// separately (view creation) and is skipped here.
static const std::vector<std::vector<std::string>> tpch_deterministic_params = {
    {"'1998-09-02'"},
    {"15", "'%BRASS'", "'EUROPE'", "'EUROPE'"},
    {"'BUILDING'", "'1995-03-15'", "'1995-03-15'"},
    {"'1993-07-01'", "'1993-10-01'"},
    {"'ASIA'", "'1994-01-01'", "'1995-01-01'"},
    {"'1994-01-01'", "'1995-01-01'", ".06", ".06", "24"},
    {"'FRANCE'", "'GERMANY'", "'GERMANY'", "'FRANCE'", "'1995-01-01'", "'1996-12-31'"},
    {"'BRAZIL'", "'AMERICA'", "'1995-01-01'", "'1996-12-31'", "'ECONOMY ANODIZED STEEL'"},
    {"'%green%'"},
    {"'1993-10-01'", "'1994-01-01'"},
    {"'GERMANY'", "0.0001", "'GERMANY'"},
    {"'MAIL'", "'SHIP'", "'1994-01-01'", "'1995-01-01'"},
    {"'%special%requests%'"},
    {"'1995-09-01'", "'1995-10-01'"},
    {},  // Query 15: view creation, handled separately
    {"'Brand#45'", "'MEDIUM POLISHED%'", "49", "14", "23", "45", "19", "3", "36", "9"},
    {"'Brand#23'", "'MED BOX'"},
    {"300"},
    {"'Brand#12'", "1", "1", "'Brand#23'", "10", "10", "'Brand#34'", "20", "20"},
    {"'forest%'", "'1994-01-01'", "'1995-01-01'", "'CANADA'"},
    {"'SAUDI ARABIA'"},
    {"'13'", "'31'", "'23'", "'29'", "'30'", "'18'", "'17'", "'13'", "'31'", "'23'", "'29'", "'30'", "'18'", "'17'"}};

// Representative TPC-C queries (one per transaction type). Parameter values
// are dummy — only the LQP structure matters for access pattern analysis.
static const std::vector<std::string> tpcc_representative_queries = {
    // NEW_ORDER: point lookups on warehouse, district, customer, item, stock
    "SELECT * FROM CUSTOMER WHERE C_W_ID = 1 AND C_D_ID = 1 AND C_ID = 1;",
    "SELECT * FROM WAREHOUSE WHERE W_ID = 1;",
    "SELECT * FROM DISTRICT WHERE D_W_ID = 1 AND D_ID = 1;",
    "SELECT * FROM STOCK WHERE S_I_ID = 1 AND S_W_ID = 1;",
    "SELECT * FROM ITEM WHERE I_ID = 1;",
    // PAYMENT: point lookups on warehouse, district, customer
    "SELECT * FROM \"ORDER\" WHERE O_W_ID = 1 AND O_D_ID = 1 AND O_ID = 1;",
    "SELECT * FROM ORDER_LINE WHERE OL_W_ID = 1 AND OL_D_ID = 1 AND OL_O_ID = 1;",
    "SELECT * FROM NEW_ORDER WHERE NO_W_ID = 1 AND NO_D_ID = 1;",
    // STOCK_LEVEL: range scan on order_line, join with stock
    "SELECT COUNT(*) FROM ORDER_LINE OL JOIN STOCK S ON S_I_ID = OL_OL_I_ID "
    "WHERE OL_W_ID = 1 AND OL_D_ID = 1 AND OL_O_ID >= 1 AND S_W_ID = 1 AND S_QUANTITY < 10;"};

// ─────────────────────────────────────────────────────────────────────────────
// Shared all-storage base enumeration (EDBT v4 B2).
// Enumerates every (table, chunk, column) segment of the loaded store, mirroring
// the migration daemon's per-cycle candidate scan exactly (sealed chunks only,
// all columns). This is the candidate-set backbone for ALL policies: our_freq
// (--ablate_d1) keeps access_type=Unknown everywhere; our_full overlays the
// audited LQP signals (analyze()); cost_c3_greedy consumes the same set and
// differs only in its typed-cost benefit. Initial and daemon candidate-set
// hashes are therefore identical by construction.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<SegmentPlacementDecision> analyze_by_frequency() {
  auto decisions = std::vector<SegmentPlacementDecision>{};
  auto& storage_manager = Hyrise::get().storage_manager;

  for (const auto& [table_name, table] : storage_manager.tables()) {
    const auto chunk_count = table->chunk_count();
    const auto column_count = table->column_count();
    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      const auto& chunk = table->get_chunk(chunk_id);
      // Mirror the daemon's candidate scan: mutable chunks are never migration
      // candidates, so they are excluded here too (identical candidate set).
      if (!chunk || chunk->is_mutable()) continue;
      const auto is_immutable = !chunk->is_mutable();
      for (auto column_id = ColumnID{0}; column_id < column_count; ++column_id) {
        const auto& segment = chunk->get_segment(column_id);
        if (!segment) continue;
        auto decision = SegmentPlacementDecision{};
        decision.table_name = table_name;
        decision.chunk_id = chunk_id;
        decision.column_id = column_id;
        decision.access_type = AccessType::Unknown;  // no LQP info
        decision.access_frequency = PlanAnalyzer::get_access_frequency(segment);
        decision.is_immutable = is_immutable;
        // data_volume: set to 0 here; tag_segments() will fill it in
        decision.data_volume = 0;
        decisions.push_back(std::move(decision));
      }
    }
  }
  return decisions;
}

// ─────────────────────────────────────────────────────────────────────────────
// PlanAnalyzer::analyze — main entry point (stages 1-3)
// ─────────────────────────────────────────────────────────────────────────────
std::vector<SegmentPlacementDecision> PlanAnalyzer::analyze(AbstractBenchmarkItemRunner& runner, bool ablate_d1) {
  // [EDBT v4 B2] Shared all-storage initial candidate enumerator: the base set
  // is the full store (analyze_by_frequency, sealed chunks only) for EVERY
  // policy, so the initial candidate set and every daemon cycle's set are
  // identical by construction. Policy differences live only in per-segment
  // signals, never in the candidate set.
  auto decisions = analyze_by_frequency();

  if (!ablate_d1) {
    // our_full: overlay the audited LQP signals onto the full-store base.
    // Signals are expanded through the unchanged resolve_segments() (preserving
    // its chunk-pruning semantics) and only access_type / estimated_cardinality
    // are overwritten for referenced segments. Segments never referenced by a
    // plan REMAIN candidates with access_type=Unknown and cardinality 0.
    const auto signals = analyze_lqps();
    const auto plan_decisions = resolve_segments(signals);
    auto overlay = std::unordered_map<std::string, size_t>{};
    overlay.reserve(plan_decisions.size() * 2);
    for (auto i = size_t{0}; i < plan_decisions.size(); ++i) {
      const auto& plan_decision = plan_decisions[i];
      const auto key = plan_decision.table_name + ":" + std::to_string(plan_decision.chunk_id) + ":" +
                       std::to_string(plan_decision.column_id);
      overlay[key] = i;
    }
    for (auto& decision : decisions) {
      const auto key = decision.table_name + ":" + std::to_string(decision.chunk_id) + ":" +
                       std::to_string(decision.column_id);
      const auto it = overlay.find(key);
      if (it == overlay.end()) continue;
      decision.access_type = plan_decisions[it->second].access_type;
      decision.estimated_cardinality = plan_decisions[it->second].estimated_cardinality;
    }
  }

  // Stage 3: Characteristic Tagger — tag with volume, frequency, immutability
  tag_segments(decisions);

  return decisions;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 1: Build SQL pipelines and traverse LQPs
// ─────────────────────────────────────────────────────────────────────────────
std::vector<PlanAnalyzer::ColumnAccessSignal> PlanAnalyzer::analyze_lqps() {
  auto signals = std::vector<ColumnAccessSignal>{};

  // Build LQPs from both TPC-H and TPC-C (whichever tables are loaded)
  auto lqp_roots = std::vector<std::shared_ptr<AbstractLQPNode>>{};

  // Check which workload is loaded by looking at table names
  auto& storage_manager = Hyrise::get().storage_manager;
  const auto& tables = storage_manager.tables();
  auto has_tpch = tables.find("lineitem") != tables.end();
  auto has_tpcc = tables.find("warehouse") != tables.end() || tables.find("WAREHOUSE") != tables.end();
  auto has_job = tables.find("title") != tables.end();

  if (has_tpch) {
    auto tpch_lqps = build_tpch_lqps();
    lqp_roots.insert(lqp_roots.end(), tpch_lqps.begin(), tpch_lqps.end());
  }

  if (has_tpcc) {
    auto tpcc_lqps = build_tpcc_lqps();
    lqp_roots.insert(lqp_roots.end(), tpcc_lqps.begin(), tpcc_lqps.end());
  }

  if (has_job) {
    auto job_lqps = build_job_lqps();
    lqp_roots.insert(lqp_roots.end(), job_lqps.begin(), job_lqps.end());
  }

  if (lqp_roots.empty()) {
    // Fallback: analyze all tables by frequency
    return signals;
  }

  // Traverse each LQP to extract column access signals
  for (const auto& root : lqp_roots) {
    if (!root) continue;
    traverse_lqp(root, signals);
  }

  // Deduplicate: merge signals with same (table_name, column_id)
  // by incrementing repetition count (reflected in access_frequency from profiling)
  auto merged = std::vector<ColumnAccessSignal>{};
  auto seen = std::unordered_map<std::string, size_t>{};
  for (auto& sig : signals) {
    auto key = sig.table_name + ":" + std::to_string(sig.column_id);
    if (seen.count(key)) {
      // Already seen this table+column; keep the most specific access type
      auto& existing = merged[seen[key]];
      // Prefer more specific types (PointLookup > Scan > Unknown)
      if (existing.access_type == AccessType::Unknown && sig.access_type != AccessType::Unknown) {
        existing.access_type = sig.access_type;
      }
      // Take max cardinality estimate
      if (sig.estimated_cardinality > existing.estimated_cardinality) {
        existing.estimated_cardinality = sig.estimated_cardinality;
      }
    } else {
      seen[key] = merged.size();
      merged.push_back(sig);
    }
  }

  return merged;
}

std::vector<std::shared_ptr<AbstractLQPNode>> PlanAnalyzer::build_tpch_lqps() {
  auto lqp_roots = std::vector<std::shared_ptr<AbstractLQPNode>>{};

  for (auto query_id = size_t{1}; query_id <= 22; ++query_id) {
    // Skip query 15 (view creation — handled specially in TPC-H)
    if (query_id == 15) continue;

    const auto query_it = tpch_query_strings.find(query_id);
    if (query_it == tpch_query_strings.end()) continue;

    auto query_template = std::string{query_it->second};

    // Substitute ? placeholders with deterministic parameter values
    const auto& params = tpch_deterministic_params[query_id - 1];
    for (const auto& param : params) {
      boost::replace_first(query_template, "?", param);
    }

    // Build SQL pipeline and get optimized LQP
    try {
      auto pipeline = SQLPipelineBuilder{query_template}.create_pipeline();
      const auto& optimized_lqps = pipeline.get_optimized_logical_plans();
      for (const auto& lqp : optimized_lqps) {
        if (lqp) lqp_roots.push_back(lqp);
      }
    } catch (const std::exception& e) {
      std::cout << "- [Δ1] Warning: failed to build LQP for TPC-H Q" << query_id << ": " << e.what() << "\n";
    }
  }

  return lqp_roots;
}

std::vector<std::shared_ptr<AbstractLQPNode>> PlanAnalyzer::build_tpcc_lqps() {
  auto lqp_roots = std::vector<std::shared_ptr<AbstractLQPNode>>{};

  for (const auto& sql : tpcc_representative_queries) {
    try {
      auto pipeline = SQLPipelineBuilder{sql}.create_pipeline();
      const auto& optimized_lqps = pipeline.get_optimized_logical_plans();
      for (const auto& lqp : optimized_lqps) {
        if (lqp) lqp_roots.push_back(lqp);
      }
    } catch (const std::exception& e) {
      std::cout << "- [Δ1] Warning: failed to build TPC-C LQP: " << e.what() << "\n";
    }
  }

  return lqp_roots;
}

// ─────────────────────────────────────────────────────────────────────────────
// Build SQL pipelines for JOB (Join Order Benchmark) queries.
// Reads all .sql files from third_party/join-order-benchmark/ (excluding
// schema.sql and fkindexes.sql) and builds optimized LQPs.
// Unlike TPC-H, JOB queries already contain literal parameter values, so
// no placeholder substitution is needed.
// ─────────────────────────────────────────────────────────────────────────────
std::vector<std::shared_ptr<AbstractLQPNode>> PlanAnalyzer::build_job_lqps() {
  auto lqp_roots = std::vector<std::shared_ptr<AbstractLQPNode>>{};

  const auto query_dir = std::filesystem::path(_query_dir);  // [IMPL-5] overridable
  if (!std::filesystem::exists(query_dir)) {
    std::cout << "- [Δ1] JOB query directory not found: " << query_dir << "\n";
    return lqp_roots;
  }

  // Collect and sort .sql file paths for deterministic ordering
  auto sql_files = std::vector<std::filesystem::path>{};
  for (const auto& entry : std::filesystem::directory_iterator(query_dir)) {
    if (!entry.is_regular_file()) continue;
    const auto& path = entry.path();
    if (path.extension() != ".sql") continue;
    const auto stem = path.stem().string();
    if (stem == "schema" || stem == "fkindexes") continue;
    sql_files.push_back(path);
  }
  std::sort(sql_files.begin(), sql_files.end());

  // [IMPL-5] evidence line: which query dir plan analysis followed (R11
  // construct gate for E10 asserts this equals the benchmark --query_path).
  std::cout << "- [IMPL-5] plan-analysis query dir: " << _query_dir << "\n";
  std::cout << "- [Δ1] Building LQPs for " << sql_files.size() << " JOB queries\n";

  for (const auto& sql_file : sql_files) {
    // Read the entire .sql file
    auto ifstream = std::ifstream(sql_file);
    if (!ifstream.is_open()) {
      std::cout << "- [Δ1] Warning: cannot open " << sql_file << "\n";
      continue;
    }
    auto sql = std::string{};
    auto line = std::string{};
    while (std::getline(ifstream, line)) {
      // Strip trailing \r in case of CRLF line endings (some JOB .sql files
      // may have Windows line terminators that confuse the SQL parser).
      if (!line.empty() && line.back() == '\r') line.pop_back();
      sql += line + "\n";
    }

    if (sql.empty()) continue;

    try {
      auto pipeline = SQLPipelineBuilder{sql}.create_pipeline();
      const auto& optimized_lqps = pipeline.get_optimized_logical_plans();
      for (const auto& lqp : optimized_lqps) {
        if (lqp) lqp_roots.push_back(lqp);
      }
    } catch (const std::exception& e) {
      std::cout << "- [Δ1] Warning: failed to build LQP for " << sql_file.filename() << ": " << e.what() << "\n";
    }
  }

  std::cout << "- [Δ1] Built " << lqp_roots.size() << " JOB LQP roots\n";
  return lqp_roots;
}

// ─────────────────────────────────────────────────────────────────────────────
// Traverse a single LQP root, extracting column access signals
// ─────────────────────────────────────────────────────────────────────────────
void PlanAnalyzer::traverse_lqp(const std::shared_ptr<AbstractLQPNode>& root,
                                std::vector<ColumnAccessSignal>& signals) {
  // Find all StoredTableNodes in this LQP
  auto stored_table_nodes = lqp_find_nodes_by_type(root, LQPNodeType::StoredTable);

  for (const auto& node : stored_table_nodes) {
    auto stored_table_node = std::dynamic_pointer_cast<StoredTableNode>(node);
    if (!stored_table_node) continue;

    const auto& table_name = stored_table_node->table_name;

    // Get pruned chunk IDs (chunks that will NOT be accessed)
    const auto& pruned_chunks = stored_table_node->pruned_chunk_ids();
    const auto& pruned_cols = stored_table_node->pruned_column_ids();

    // Classify the access type by walking upward to the consuming operator.
    // EDBT v4 Phase B (B1): with the trace flag on we additionally record the
    // consuming operator's LQP node type per column (observation-only).
    auto consumer_name = std::string{};
    const auto trace_on = Hyrise::get().placement_options.placement_trace;
    const auto access_type = classify_access_type(stored_table_node, trace_on ? &consumer_name : nullptr);

    // Estimate cardinality from the node's statistics
    // [IMPL-4] (E9B): multiply the optimizer cardinality estimate by kappa at
    // this read boundary (the ONLY place plan analysis touches estimates).
    auto cardinality = 0.0f;
    if (stored_table_node->table_statistics) {
      cardinality = stored_table_node->table_statistics->row_count * _kappa;
    }

    // Determine which columns are accessed (all - pruned)
    auto& storage_manager = Hyrise::get().storage_manager;
    if (!storage_manager.has_table(table_name)) continue;
    const auto& table = storage_manager.get_table(table_name);
    const auto column_count = table->column_count();

    // Build a set of pruned column IDs for fast lookup
    auto pruned_col_set = std::set<ColumnID>(pruned_cols.begin(), pruned_cols.end());

    for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
      if (pruned_col_set.count(col_id)) continue;  // pruned → not accessed

      if (trace_on) {
        // EDBT v4 Phase B (B1): observation-only operator mask accumulation.
        _column_operator_names[table_name + ":" + std::to_string(col_id)].insert(consumer_name);
      }

      auto signal = ColumnAccessSignal{};
      signal.table_name = table_name;
      signal.column_id = col_id;
      signal.access_type = access_type;
      signal.estimated_cardinality = cardinality;
      signal.pruned_chunk_ids = pruned_chunks;
      signals.push_back(std::move(signal));
    }
  }
}

// ─────────────────────────────────────────────────────────────────────────────
// Classify access type by walking upward from StoredTableNode to the first
// consuming operator (Predicate/Join/Aggregate/Sort), skipping Projection,
// Validate, Limit, Alias nodes.
// EDBT v4 Phase B (B1): optional consumer_name out-param (observation-only,
// used by the operator trace / operator_mask). Passing nullptr keeps the
// placement path bit-identical to the previous single-return form.
// ─────────────────────────────────────────────────────────────────────────────
AccessType PlanAnalyzer::classify_access_type(const std::shared_ptr<AbstractLQPNode>& stored_table_node,
                                              std::string* consumer_name) {
  // Walk upward via outputs()
  auto current = stored_table_node;
  auto visited = std::set<std::shared_ptr<AbstractLQPNode>>{};

  const auto set_consumer = [consumer_name](const char* name) {
    if (consumer_name) *consumer_name = name;
  };

  while (!current->outputs().empty()) {
    auto found_consumer = false;
    for (const auto& output : current->outputs()) {
      if (!output || visited.count(output)) continue;
      visited.insert(output);

      switch (output->type) {
        case LQPNodeType::Predicate: {
          set_consumer("Predicate");
          // Check if it's a point lookup (Equals) or a range scan
          auto pred_node = std::dynamic_pointer_cast<PredicateNode>(output);
          if (pred_node && pred_node->predicate()) {
            auto binary_pred = std::dynamic_pointer_cast<BinaryPredicateExpression>(pred_node->predicate());
            if (binary_pred && binary_pred->predicate_condition == PredicateCondition::Equals) {
              return AccessType::PointLookup;
            }
          }
          return AccessType::Scan;
        }
        case LQPNodeType::Join: {
          set_consumer("Join");
          // Determine if this table is on the build (left) or probe (right) side
          auto join_node = std::dynamic_pointer_cast<JoinNode>(output);
          if (join_node) {
            if (output->left_input() == current) {
              return AccessType::HashBuild;
            } else {
              return AccessType::HashProbe;
            }
          }
          return AccessType::Scan;
        }
        case LQPNodeType::Aggregate:
          set_consumer("Aggregate");
          return AccessType::Aggregate;
        case LQPNodeType::Sort:
          set_consumer("Sort");
          return AccessType::Sort;
        case LQPNodeType::Projection:
        case LQPNodeType::Validate:
        case LQPNodeType::Limit:
        case LQPNodeType::Alias:
          // Skip these — continue walking upward
          current = output;
          found_consumer = true;
          break;
        default:
          set_consumer("Other");
          // Unknown operator type — treat as generic scan
          return AccessType::Scan;
      }
      if (found_consumer) break;
    }
    if (!found_consumer) break;
  }

  set_consumer("None");
  return AccessType::Unknown;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 2: Object Resolver — expand signals to concrete segment lists
// ─────────────────────────────────────────────────────────────────────────────
std::vector<SegmentPlacementDecision> PlanAnalyzer::resolve_segments(const std::vector<ColumnAccessSignal>& signals) {
  auto decisions = std::vector<SegmentPlacementDecision>{};
  auto& storage_manager = Hyrise::get().storage_manager;

  for (const auto& signal : signals) {
    if (!storage_manager.has_table(signal.table_name)) continue;
    const auto& table = storage_manager.get_table(signal.table_name);
    const auto chunk_count = table->chunk_count();

    // Build a set of pruned chunk IDs for fast lookup
    auto pruned_set = std::set<ChunkID>(signal.pruned_chunk_ids.begin(), signal.pruned_chunk_ids.end());

    for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
      // Skip pruned chunks (they will NOT be accessed — key Δ1 advantage)
      if (pruned_set.count(chunk_id)) continue;

      auto decision = SegmentPlacementDecision{};
      decision.table_name = signal.table_name;
      decision.chunk_id = chunk_id;
      decision.column_id = signal.column_id;
      decision.access_type = signal.access_type;
      decision.estimated_cardinality = signal.estimated_cardinality;
      decisions.push_back(std::move(decision));
    }
  }

  return decisions;
}

// ─────────────────────────────────────────────────────────────────────────────
// Stage 3: Characteristic Tagger — tag each segment with volume, frequency,
// is_immutable
// ─────────────────────────────────────────────────────────────────────────────
void PlanAnalyzer::tag_segments(std::vector<SegmentPlacementDecision>& decisions) {
  auto& storage_manager = Hyrise::get().storage_manager;

  for (auto& decision : decisions) {
    if (!storage_manager.has_table(decision.table_name)) continue;
    const auto& table = storage_manager.get_table(decision.table_name);
    const auto& chunk = table->get_chunk(decision.chunk_id);
    if (!chunk) continue;

    const auto& segment = chunk->get_segment(decision.column_id);
    if (!segment) continue;

    // Access frequency from SegmentAccessCounter
    decision.access_frequency = get_access_frequency(segment);

    // Immutability (key for Δ3: immutable = zero-cost replica consistency)
    decision.is_immutable = !chunk->is_mutable();

    // Data volume: approximate from row count × estimated column width
    // For a more precise estimate, we'd use segment->memory_size() but that's
    // not universally available. Approximate with chunk size × 8 bytes.
    decision.data_volume = static_cast<uint64_t>(chunk->size()) * 8;
  }
}

uint64_t PlanAnalyzer::get_access_frequency(const std::shared_ptr<AbstractSegment>& segment) {
  // Sum all access counter types (Point + Sequential + Monotonic + Random + Dictionary)
  const auto& counter = segment->access_counter;
  return counter[SegmentAccessCounter::AccessType::Point].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Sequential].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Monotonic].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Random].load(std::memory_order_relaxed) +
         counter[SegmentAccessCounter::AccessType::Dictionary].load(std::memory_order_relaxed);
}

// ─────────────────────────────────────────────────────────────────────────────
// EDBT v4 Phase B (commit B1): operator mask lookup (observation-only)
// ─────────────────────────────────────────────────────────────────────────────
const std::string& PlanAnalyzer::operator_mask(const std::string& table_name, ColumnID column_id) const {
  static const std::string no_mask = "-";
  const auto key = table_name + ":" + std::to_string(column_id);
  const auto it = _column_operator_names.find(key);
  if (it == _column_operator_names.end() || it->second.empty()) return no_mask;
  const auto cached = _column_operator_masks.find(key);
  if (cached != _column_operator_masks.end()) return cached->second;
  auto joined = std::string{};
  for (const auto& name : it->second) {
    if (!joined.empty()) joined += "|";
    joined += name;
  }
  return _column_operator_masks.emplace(key, std::move(joined)).first->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// EDBT v4 Phase B (commit B1): operator/segment reference trace
// (schemas/operator_segment_trace.schema.json, plan_reference_only rows).
// Per-execution attribution is not measurable under clients=10 concurrency and
// the fork carries no hardware traffic counters per operator, so every row
// declares attribution_level=plan_reference_only and leaves the measured
// traffic fields (actual_reads, dram/cxl accesses/bytes) empty. Rows connect
// each JOB query's consuming operators to the segments they reference
// (chunk-pruning-aware). operator_wall_ns / rows_in / rows_out are 0 = not
// measured (declared in the Phase B POLICY_SPEC).
// ─────────────────────────────────────────────────────────────────────────────
void PlanAnalyzer::write_operator_reference_trace(const std::string& path, const std::string& run_id,
                                                  const std::string& config, uint32_t rep) {
  auto& storage_manager = Hyrise::get().storage_manager;
  const auto has_job = storage_manager.has_table("title");
  if (!has_job) {
    std::cout << "- [EDBT-TRACE] operator reference trace skipped (no JOB tables loaded)\n";
    return;
  }

  const auto query_dir = std::filesystem::path(_query_dir);
  if (!std::filesystem::exists(query_dir)) {
    std::cout << "- [EDBT-TRACE] operator reference trace skipped (query dir not found: " << query_dir << ")\n";
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

  auto ofs = std::ofstream(path);
  if (!ofs.is_open()) {
    std::cout << "- [EDBT-TRACE] cannot open operator trace output " << path << "\n";
    return;
  }
  ofs << "run_id,config,rep,query_name,execution_id,operator_id,operator_type,table_name,"
         "chunk_id,column_id,estimated_rows,rows_in,rows_out,operator_wall_ns,actual_reads,"
         "tier_at_execution,attribution_level,dram_accesses,cxl_accesses,dram_bytes,cxl_bytes\n";

  auto total_rows = size_t{0};
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
    try {
      auto pipeline = SQLPipelineBuilder{sql}.create_pipeline();
      const auto& optimized_lqps = pipeline.get_optimized_logical_plans();
      auto operator_id = size_t{0};
      for (const auto& lqp : optimized_lqps) {
        if (!lqp) continue;
        const auto stored_table_nodes = lqp_find_nodes_by_type(lqp, LQPNodeType::StoredTable);
        for (const auto& node : stored_table_nodes) {
          auto stored_table_node = std::dynamic_pointer_cast<StoredTableNode>(node);
          if (!stored_table_node) continue;
          const auto& table_name = stored_table_node->table_name;
          if (!storage_manager.has_table(table_name)) continue;
          const auto& table = storage_manager.get_table(table_name);

          auto consumer_name = std::string{};
          const auto access_type = classify_access_type(stored_table_node, &consumer_name);
          (void)access_type;  // operator_type carries the raw consumer; atype stays a placement signal

          auto cardinality = 0.0f;
          if (stored_table_node->table_statistics) {
            cardinality = stored_table_node->table_statistics->row_count * _kappa;
          }

          const auto& pruned_chunks = stored_table_node->pruned_chunk_ids();
          const auto pruned_chunk_set = std::set<ChunkID>(pruned_chunks.begin(), pruned_chunks.end());
          const auto& pruned_cols = stored_table_node->pruned_column_ids();
          const auto pruned_col_set = std::set<ColumnID>(pruned_cols.begin(), pruned_cols.end());

          const auto chunk_count = table->chunk_count();
          const auto column_count = table->column_count();
          for (auto chunk_id = ChunkID{0}; chunk_id < chunk_count; ++chunk_id) {
            if (pruned_chunk_set.count(chunk_id)) continue;
            for (auto col_id = ColumnID{0}; col_id < column_count; ++col_id) {
              if (pruned_col_set.count(col_id)) continue;
              ofs << run_id << "," << config << "," << rep << "," << query_name << ",0," << operator_id
                  << "," << consumer_name << "," << table_name << "," << chunk_id << "," << col_id << ","
                  << cardinality << ",0,0,0,,UNKNOWN,plan_reference_only,,,,\n";
              ++total_rows;
            }
          }
          ++operator_id;
        }
      }
    } catch (const std::exception& e) {
      std::cout << "- [EDBT-TRACE] warning: no operator rows for " << query_name << ": " << e.what() << "\n";
    }
  }

  std::cout << "- [EDBT-TRACE] operator reference trace: " << total_rows << " plan_reference_only rows ("
            << sql_files.size() << " queries) -> " << path << "\n";
}

}  // namespace hyrise
