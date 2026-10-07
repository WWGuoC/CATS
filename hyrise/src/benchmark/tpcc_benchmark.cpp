#include <algorithm>
#include <string>
#include <vector>

#include <boost/algorithm/string.hpp>

#include "benchmark_runner.hpp"
#include "cli_config_parser.hpp"
#include "hyrise.hpp"
#include "magic_enum.hpp"
#include "sql/sql_pipeline_builder.hpp"
#include "tpcc/constants.hpp"
#include "tpcc/tpcc_benchmark_item_runner.hpp"
#include "tpcc/tpcc_table_generator.hpp"
#include "utils/assert.hpp"

using namespace hyrise;  // NOLINT

/**
 * This benchmark measures Hyrise's performance executing the TPC-C benchmark. As with the other TPC-* benchmarks, we
 * took some liberty in interpreting the standard. Most notably, all parts about the simulated terminals are ignored.
 * Instead, only the queries leading to the terminal output are executed. In the research world, doing so has become
 * the de facto standard.
 *
 * Other limitations (that may be removed in the future):
 *  - No primary / foreign keys are used as they are currently unsupported
 *  - Values that are "retrieved" by the terminal are just selected, but not necessarily materialized
 *  - Data is not persisted as logging is currently unsupported; this means that the durability tests are not executed
 *  - As decimals are not supported, we use floats instead
 *  - The delivery transaction is not executed in a "deferred" mode; as such, no delivery result file is written
 *  - We do not execute the isolation tests, as we consider our MVCC tests to be sufficient
 *
 * Most importantly, we do not claim to report correctly calculated tpmC.
 *
 * main() is mostly concerned with parsing the CLI options while BenchmarkRunner.run() performs the actual benchmark
 * logic.
 */

namespace {
void check_consistency(const size_t num_warehouses);
}

int main(int argc, char* argv[]) {
  auto cli_options = BenchmarkRunner::get_basic_cli_options("TPC-C Benchmark");

  // clang-format off
  cli_options.add_options()
    // We use -s instead of -w for consistency with the options of our other TPC-x binaries.
    ("s,scale", "Scale factor (warehouses)", cxxopts::value<size_t>()->default_value("10"))
    ("consistency_checks", "Run TPC-C consistency checks after benchmark (included with --verify)", cxxopts::value<bool>()->default_value("false")) // NOLINT(whitespace/line_length)
    ("lmem_ids", "Specify the local memory NUMA nodes (comma-separated, e.g., --lmem_ids 0,2)", cxxopts::value<std::string>()->default_value("0")) // NOLINT
    ("rmem_ids", "Specify the remote memory NUMA nodes (comma-separated, e.g., --rmem_ids 1,3,4)", cxxopts::value<std::string>()) // NOLINT
    ("rmem_weights", "Specify the remote memory NUMA node weights (comma-separated, e.g., --rmem_weights 2,1,1)", cxxopts::value<std::string>()) // NOLINT
    ("fixed_columns", "Specify the number of most frequently accessed columns fixed in local memory", cxxopts::value<uint32_t>()->default_value("0")) // NOLINT
    ("semantic", "Use semantic proactive placement plugin (d1+d2+d3)", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d1", "Disable d1: use access-frequency instead of LQP plan analysis", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d2", "Disable d2: column-level migration instead of segment-level", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d3_replica", "Disable d3 replicas: single-copy migration", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d3_gc", "Disable d3 semantic GC: LRU fallback only", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("dram_first", "DRAM-first mode: data starts on DRAM, demote cold segments to CXL", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("daemon_period", "Daemon period in seconds (0 = no daemon)", cxxopts::value<uint32_t>()->default_value("5")) // NOLINT
    ("demote_pct", "Fraction of cold segments to demote to CXL in pre-placement (0-1)", cxxopts::value<float>()->default_value("0.3")) // NOLINT
    ("dram_capacity_gb", "DRAM buffer size in GB (0 = auto, derived from scale factor)", cxxopts::value<float>()->default_value("0")) // NOLINT
    ("cxl_capacity_gb", "CXL buffer size in GB (0 = auto, derived from scale factor)", cxxopts::value<float>()->default_value("0")); // NOLINT
  // clang-format on

  auto config = std::shared_ptr<BenchmarkConfig>{};
  auto num_warehouses = size_t{0};
  auto consistency_checks = false;
  auto comma_separated_lmem_ids = std::string{};
  auto comma_separated_rmem_ids = std::string{};
  auto comma_separated_rmem_weights = std::string{};
  auto fixed_local_columns = uint32_t{};
  auto semantic = false;
  auto ablate_d1 = false;
  auto ablate_d2 = false;
  auto ablate_d3_replica = false;
  auto ablate_d3_gc = false;
  auto dram_first = false;
  auto daemon_period = uint32_t{5};
  auto demote_pct = 0.3f;
  auto dram_capacity_gb = 0.0f;
  auto cxl_capacity_gb = 0.0f;

  // Parse command line args
  const auto cli_parse_result = cli_options.parse(argc, argv);

  if (CLIConfigParser::print_help_if_requested(cli_options, cli_parse_result)) {
    return 0;
  }

  num_warehouses = cli_parse_result["scale"].as<size_t>();
  consistency_checks = cli_parse_result["consistency_checks"].as<bool>();

  if (cli_parse_result.count("lmem_ids")) {
    comma_separated_lmem_ids = cli_parse_result["lmem_ids"].as<std::string>();
  }
  if (cli_parse_result.count("rmem_ids")) {
    comma_separated_rmem_ids = cli_parse_result["rmem_ids"].as<std::string>();
  }
  if (cli_parse_result.count("rmem_weights")) {
    comma_separated_rmem_weights = cli_parse_result["rmem_weights"].as<std::string>();
  }
  if (cli_parse_result.count("fixed_columns")) {
    fixed_local_columns = cli_parse_result["fixed_columns"].as<uint32_t>();
  }
  semantic = cli_parse_result["semantic"].as<bool>();
  ablate_d1 = cli_parse_result["ablate_d1"].as<bool>();
  ablate_d2 = cli_parse_result["ablate_d2"].as<bool>();
  ablate_d3_replica = cli_parse_result["ablate_d3_replica"].as<bool>();
  ablate_d3_gc = cli_parse_result["ablate_d3_gc"].as<bool>();
  dram_first = cli_parse_result["dram_first"].as<bool>();
  daemon_period = cli_parse_result["daemon_period"].as<uint32_t>();
  demote_pct = cli_parse_result["demote_pct"].as<float>();
  dram_capacity_gb = cli_parse_result["dram_capacity_gb"].as<float>();
  cxl_capacity_gb = cli_parse_result["cxl_capacity_gb"].as<float>();

  config = CLIConfigParser::parse_cli_options(cli_parse_result);

  // As TPC-C procedures may run into conflicts on both the Hyrise and the SQLite side, we cannot guarantee that the
  // two databases stay in sync.
  Assert(!config->verify || config->clients == 1, "Cannot run verification with more than one client.");

  auto context = BenchmarkRunner::create_context(*config);

  std::cout << "- TPC-C scale factor (number of warehouses) is " << num_warehouses << '\n';

  // Parse lmem node IDs
  auto lmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_lmem_ids.empty()) {
    auto local_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_lmem_ids, boost::is_any_of(","));
    boost::split(local_numa_nodes_str, comma_separated_lmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(local_numa_nodes_str.begin(), local_numa_nodes_str.end(), std::back_inserter(lmem_node_ids),
                   [](const auto& node_id_str) { return static_cast<NumaNodeID>(std::stoul(node_id_str)); });
  }

  // Parse rmem node IDs
  auto rmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_rmem_ids.empty()) {
    auto remote_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_rmem_ids, boost::is_any_of(","));
    boost::split(remote_numa_nodes_str, comma_separated_rmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(remote_numa_nodes_str.begin(), remote_numa_nodes_str.end(), std::back_inserter(rmem_node_ids),
                   [](const auto& node_id_str) { return static_cast<NumaNodeID>(std::stoul(node_id_str)); });
  }

  // Parse rmem weights
  auto rmem_weights = InterleavingWeights{};
  if (!comma_separated_rmem_weights.empty()) {
    auto rmem_weights_str = std::vector<std::string>();
    boost::trim_if(comma_separated_rmem_weights, boost::is_any_of(","));
    boost::split(rmem_weights_str, comma_separated_rmem_weights, boost::is_any_of(","), boost::token_compress_on);
    std::transform(rmem_weights_str.begin(), rmem_weights_str.end(), std::back_inserter(rmem_weights),
                   [](const auto& weight_str) { return static_cast<uint32_t>(std::stoul(weight_str)); });
  }

  // Type selection
  const auto type = [&]() {
    if (semantic) {
      return PlacementType::SemanticProactive;
    }
    if (fixed_local_columns > 0) {
      return PlacementType::AccessFrequency;
    }
    if (rmem_node_ids.size() > 0) {
      if (rmem_weights.size() > 0) {
        return PlacementType::PagesWeightedInterleaved;
      }
      return PlacementType::PagesRoundRobinInterleaved;
    }
    return PlacementType::None;
  }();
  std::cout << "- Data placement type: " << magic_enum::enum_name(type) << "\n";
  if (semantic) {
    std::cout << "- Semantic placement: ON (ablate d1=" << ablate_d1 << ", d2=" << ablate_d2
              << ", d3_replica=" << ablate_d3_replica << ", d3_gc=" << ablate_d3_gc << ")\n";
    if (dram_first) {
      std::cout << "- DRAM-first mode: ON (demote_pct=" << demote_pct
                << ", daemon_period=" << daemon_period << "s)\n";
    }
  }
  Hyrise::get().placement_options = {.scale_factor = static_cast<float>(num_warehouses),
                                     .lmem_node_ids = lmem_node_ids,
                                     .rmem_node_ids = rmem_node_ids,
                                     .rmem_weights = rmem_weights,
                                     .type = type,
                                     .num_most_frequently_columns_local = fixed_local_columns,
                                     .json_path = *config->output_file_path,
                                     .ablate_d1 = ablate_d1,
                                     .ablate_d2 = ablate_d2,
                                     .ablate_d3_replica = ablate_d3_replica,
                                     .ablate_d3_gc = ablate_d3_gc,
                                     .dram_first = dram_first,
                                     .daemon_period_seconds = daemon_period,
                                     .demote_pct = demote_pct,
                                     .dram_capacity_gb = dram_capacity_gb,
                                     .cxl_capacity_gb = cxl_capacity_gb};

  // Add TPC-C-specific information
  context.emplace("scale_factor", num_warehouses);

  // Run the benchmark
  auto item_runner = std::make_unique<TPCCBenchmarkItemRunner>(config, num_warehouses);
  BenchmarkRunner(*config, std::move(item_runner), std::make_unique<TPCCTableGenerator>(num_warehouses, config),
                  context)
      .run();

  if (consistency_checks || config->verify) {
    std::cout << "- Running consistency checks at the end of the benchmark\n";
    check_consistency(num_warehouses);
    std::cout << "- Consistency checks passed\n";
  }
}

namespace {
template <typename T, typename = std::enable_if<std::is_floating_point_v<T>>>
bool floats_near(T a, T b) {
  if (a == b) {
    return true;
  }
  // Tolerate 0.1% discrepancy due to float variations
  return std::max(a, b) / std::min(a, b) <= 1.001;
}

void check_consistency(const size_t num_warehouses) {
  // new_order_counts[5-1][2-1] will hold the number of new_orders for W_ID 5, D_ID 2.
  // Filled as a byproduct of check 2, validated in check 3.
  auto new_order_counts =
      std::vector<std::vector<int64_t>>(num_warehouses, std::vector<int64_t>(NUM_DISTRICTS_PER_WAREHOUSE));

  const auto total_num_districts = static_cast<size_t>(num_warehouses * NUM_DISTRICTS_PER_WAREHOUSE);

  {
    std::cout << "  -> Running consistency check 1\n";  // see 3.3.2.1
    // There is only one value for W_YTD. We select MAX() because we need an aggregate column.
    auto pipeline =
        SQLPipelineBuilder{
            "SELECT W_ID, MAX(W_YTD), SUM(D_YTD) FROM WAREHOUSE, DISTRICT WHERE W_ID = D_W_ID GROUP BY W_ID"}
            .create_pipeline();
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() == static_cast<size_t>(num_warehouses), "Lost a warehouse.");
    const auto row_count = table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      const auto w_ytd = double{*table->get_value<float>(ColumnID{1}, row_id)};
      const auto d_ytd_sum = *table->get_value<double>(ColumnID{2}, row_id);

      Assert(floats_near(w_ytd, d_ytd_sum), "Mismatching YTD for WAREHOUSE and DISTRICT.");
    }
  }

  {
    std::cout << "  -> Running consistency check 2\n";
    for (size_t w_id = 1; w_id <= num_warehouses; ++w_id) {
      auto district_pipeline = SQLPipelineBuilder{std::string{"SELECT D_NEXT_O_ID - 1 FROM DISTRICT WHERE D_W_ID = "} +
                                                  std::to_string(w_id) + " ORDER BY D_ID"}
                                   .create_pipeline();
      const auto [district_pipeline_status, district_table] = district_pipeline.get_result_table();
      Assert(district_table && district_table->row_count() == NUM_DISTRICTS_PER_WAREHOUSE, "Lost a district.");
      for (auto d_id = 1; d_id <= NUM_DISTRICTS_PER_WAREHOUSE; ++d_id) {
        const auto district_max_o_id = *district_table->get_value<int32_t>(ColumnID{0}, d_id - 1);

        auto order_pipeline = SQLPipelineBuilder{std::string{"SELECT MAX(O_ID) FROM \"ORDER\" WHERE O_W_ID = "} +
                                                 std::to_string(w_id) + " AND O_D_ID = " + std::to_string(d_id)}
                                  .create_pipeline();
        const auto [order_pipeline_status, order_table] = order_pipeline.get_result_table();
        Assert(order_table && order_table->row_count() == 1, "Did not find MAX(O_ID)");
        Assert(*order_table->get_value<int32_t>(ColumnID{0}, 0) == district_max_o_id, "Mismatching order IDs.");

        auto new_order_pipeline =
            SQLPipelineBuilder{std::string{"SELECT COUNT(*), MAX(NO_O_ID) FROM NEW_ORDER WHERE NO_W_ID = "} +
                               std::to_string(w_id) + " AND NO_D_ID = " + std::to_string(d_id)}
                .create_pipeline();
        const auto [new_order_pipeline_status, new_order_table] = new_order_pipeline.get_result_table();
        Assert(order_table && order_table->row_count() == 1, "Could not retrieve new_orders.");
        const auto new_order_count = *new_order_table->get_value<int64_t>(ColumnID{0}, 0);
        new_order_counts[w_id - 1][d_id - 1] = new_order_count;
        if (new_order_count > 0) {
          Assert(*new_order_table->get_value<int32_t>(ColumnID{1}, 0) == district_max_o_id, "Mismatching order IDs.");
        }
      }
    }
  }

  {
    std::cout << "  -> Running consistency check 3\n";
    auto new_order_pipeline =
        SQLPipelineBuilder{
            "SELECT NO_W_ID, NO_D_ID, MIN(NO_O_ID), MAX(NO_O_ID) FROM NEW_ORDER GROUP BY NO_W_ID, NO_D_ID"}
            .create_pipeline();
    const auto [new_order_pipeline_status, new_order_table] = new_order_pipeline.get_result_table();
    Assert(new_order_table, "Could not retrieve new_orders.");
    const auto& row_count = new_order_table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      const auto w_id = *new_order_table->get_value<int32_t>(ColumnID{0}, row_id);
      const auto d_id = *new_order_table->get_value<int32_t>(ColumnID{1}, row_id);
      const auto min_o_id = *new_order_table->get_value<int32_t>(ColumnID{2}, row_id);
      const auto max_o_id = *new_order_table->get_value<int32_t>(ColumnID{3}, row_id);
      Assert(max_o_id - min_o_id + 1 == new_order_counts[w_id - 1][d_id - 1], "Mismatching order IDs.");
    }
  }

  {
    std::cout << "  -> Running consistency check 4\n";
    auto order_pipeline =
        SQLPipelineBuilder{
            "SELECT O_W_ID, O_D_ID, SUM(O_OL_CNT) FROM \"ORDER\" GROUP BY O_W_ID, O_D_ID ORDER BY O_W_ID, O_D_ID"}
            .create_pipeline();
    const auto [order_pipeline_status, order_table] = order_pipeline.get_result_table();
    Assert(order_table && order_table->row_count() == total_num_districts,
           "Did not find SUM(O_OL_CNT) for all districts.");

    auto order_line_pipeline =
        SQLPipelineBuilder{
            "SELECT OL_W_ID, OL_D_ID, COUNT(*) FROM ORDER_LINE GROUP BY OL_W_ID, OL_D_ID ORDER BY OL_W_ID, OL_D_ID"}
            .create_pipeline();
    const auto [order_line_pipeline_status, order_line_table] = order_line_pipeline.get_result_table();
    Assert(order_line_table && order_line_table->row_count() == total_num_districts,
           "Did not find COUNT(*) FROM ORDER_LINE for all districts.");

    const auto row_count = order_line_table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      Assert(*order_table->get_value<int64_t>(ColumnID{2}, row_id) ==
                 *order_line_table->get_value<int64_t>(ColumnID{2}, row_id),
             "Mismatching order_line count.");
    }
  }

  {
    std::cout << "  -> Running consistency check 5\n";
    // clang-format off
    auto pipeline = SQLPipelineBuilder{R"(
                      SELECT *
                      FROM "ORDER"
                      WHERE O_CARRIER_ID IS NULL
                      AND NOT EXISTS (SELECT NO_W_ID
                        FROM NEW_ORDER
                        WHERE O_W_ID = NO_W_ID
                              AND O_D_ID = NO_D_ID
                              AND NO_O_ID = O_ID)
                    )"}.create_pipeline();
    // clang-format on
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() == size_t{0}, "Found fulfilled order without O_CARRIER_ID.");
  }

  {
    std::cout << "  -> Running consistency check 6\n";
    // clang-format off
    auto pipeline = SQLPipelineBuilder{R"(
                      SELECT O_W_ID, O_D_ID, O_ID, MAX(O_OL_CNT), COUNT(*)
                      FROM "ORDER"
                      LEFT JOIN ORDER_LINE
                        ON O_W_ID = OL_W_ID
                        AND O_D_ID = OL_D_ID
                        AND O_ID = OL_O_ID
                      GROUP BY O_W_ID, O_D_ID, O_ID
                    )"}.create_pipeline();
    // clang-format on
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() > size_t{0}, "Failed to retrieve order / order lines.");
    const auto row_count = table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      Assert(*table->get_value<int32_t>(ColumnID{3}, row_id) == *table->get_value<int64_t>(ColumnID{4}, row_id),
             "Mismatching number of order lines.");
    }
  }

  {
    std::cout << "  -> Running consistency check 7\n";
    auto pipeline =
        SQLPipelineBuilder{
            "SELECT * FROM ORDER_LINE LEFT JOIN \"ORDER\" ON OL_W_ID = O_W_ID AND OL_D_ID = O_D_ID AND OL_O_ID = O_ID "
            "WHERE OL_DELIVERY_D IS NULL AND O_CARRIER_ID IS NOT NULL"}
            .create_pipeline();
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() == size_t{0},
           "Found order line without OL_DELIVERY_D even though the order was delivered.");
  }

  {
    std::cout << "  -> Running consistency check 8\n";
    auto pipeline =
        SQLPipelineBuilder{
            "SELECT W_ID, MAX(W_YTD), SUM(H_AMOUNT) FROM WAREHOUSE, HISTORY WHERE W_ID = H_W_ID GROUP BY W_ID"}
            .create_pipeline();
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() == static_cast<size_t>(num_warehouses), "Lost a warehouse.");
    const auto row_count = table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      const auto w_ytd = double{*table->get_value<float>(ColumnID{1}, row_id)};
      const auto h_amount = *table->get_value<double>(ColumnID{2}, row_id);

      Assert(floats_near(w_ytd, h_amount), "Mismatching YTD for WAREHOUSE and HISTORY.");
    }
  }

  {
    std::cout << "  -> Running consistency check 9\n";
    auto pipeline =
        SQLPipelineBuilder{
            "SELECT D_W_ID, D_ID, MAX(D_YTD), SUM(H_AMOUNT) FROM DISTRICT, HISTORY WHERE D_W_ID = H_W_ID AND D_ID "
            "= H_D_ID GROUP BY D_W_ID, D_ID"}
            .create_pipeline();
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() == total_num_districts, "Lost a district.");
    const auto row_count = table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      const auto d_ytd = double{*table->get_value<float>(ColumnID{2}, row_id)};
      const auto h_amount = *table->get_value<double>(ColumnID{3}, row_id);

      Assert(floats_near(d_ytd, h_amount), "Mismatching YTD for DISTRICT and HISTORY.");
    }
  }

  {
    std::cout << "  -> Running consistency check 10\n";
    std::cout << "  -> Skipped because of #1771\n";
    if ((false)) {
      // clang-format off
    auto pipeline = SQLPipelineBuilder{R"(
                      SELECT C_W_ID, C_D_ID, C_ID, MAX(C_BALANCE), SUM_H_AMOUNT
                        (CASE WHEN SUM_OL_AMOUNT IS NULL THEN 0 ELSE SUM_OL_AMOUNT END) AS SUM_OL_AMOUNT_NONNULL,
                      FROM CUSTOMER
                      LEFT JOIN 
                        (SELECT O_W_ID, O_D_ID, O_C_ID, SUM(OL_AMOUNT)
                         FROM "ORDER", ORDER_LINE
                         WHERE
                          OL_W_ID = O_W_ID AND OL_D_ID = O_D_ID AND OL_O_ID = O_ID
                          AND OL_DELIVERY_D IS NOT NULL
                          GROUP BY O_W_ID, O_D_ID, O_C_ID
                        ) AS sub1(O_W_ID, O_D_ID, O_C_ID, SUM_OL_AMOUNT)
                        ON O_W_ID = C_W_ID AND O_D_ID = C_D_ID AND O_C_ID = C_ID
                      LEFT JOIN
                        (SELECT H_W_ID, H_D_ID, H_C_ID, SUM(H_AMOUNT)
                         FROM HISTORY
                         GROUP BY H_W_ID, H_D_ID, H_C_ID
                        ) AS sub2(H_W_ID, H_D_ID, H_C_ID, SUM_H_AMOUNT)
                        ON H_W_ID = C_W_ID AND H_D_ID = C_D_ID AND H_C_ID = C_ID
                      GROUP BY C_W_ID, C_D_ID, C_ID, SUM_H_AMOUNT, SUM_OL_AMOUNT_NONNULL
                    )"}.create_pipeline();
      // clang-format on
      const auto [pipeline_status, table] = pipeline.get_result_table();
      Assert(table && table->row_count() == static_cast<size_t>(num_warehouses * NUM_DISTRICTS_PER_WAREHOUSE *
                                                                NUM_CUSTOMERS_PER_DISTRICT),
             "Lost a customer.");
      const auto row_count = table->row_count();
      for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
        const auto c_balance = double{*table->get_value<float>(ColumnID{3}, row_id)};
        const auto sum_h_amount = *table->get_value<double>(ColumnID{4}, row_id);
        const auto sum_ol_amount = *table->get_value<double>(ColumnID{5}, row_id);

        Assert(floats_near(sum_ol_amount - sum_h_amount, c_balance), "Mismatching amounts for customer.");
      }
    }
  }

  {
    std::cout << "  -> Running consistency check 11\n";
    std::cout << "  -> Skipped because it only relates to the unmodified database\n";
    // The consistency condition as defined in 3.3.2.11 is only valid for the initial state of the database. Once the
    // new-order and delivery transactions have executed, rows have been added to / deleted from the order and
    // new_order tables. Thus, we are skipping this check.
  }

  {
    std::cout << "  -> Running consistency check 12\n";
    // clang-format off
    auto pipeline = SQLPipelineBuilder{R"(
                      SELECT C_W_ID, C_D_ID, C_ID, C_BALANCE, C_YTD_PAYMENT,
                        (CASE WHEN SUM(OL_AMOUNT) IS NULL THEN 0 ELSE SUM(OL_AMOUNT) END) AS SUM_OL_AMOUNT_NONNULL
                      FROM CUSTOMER
                      LEFT JOIN "ORDER"
                        ON O_W_ID = C_W_ID AND O_D_ID = C_D_ID AND O_C_ID = C_ID
                      LEFT JOIN ORDER_LINE
                        ON OL_W_ID = O_W_ID AND OL_D_ID = O_D_ID AND OL_O_ID = O_ID
                        AND OL_DELIVERY_D IS NOT NULL
                      GROUP BY C_W_ID, C_D_ID, C_ID, C_BALANCE, C_YTD_PAYMENT
                    )"}.create_pipeline();
    // clang-format on
    const auto [pipeline_status, table] = pipeline.get_result_table();
    Assert(table && table->row_count() ==
                        static_cast<size_t>(num_warehouses * NUM_DISTRICTS_PER_WAREHOUSE * NUM_CUSTOMERS_PER_DISTRICT),
           "Lost a customer.");
    const auto row_count = table->row_count();
    for (auto row_id = size_t{0}; row_id < row_count; ++row_id) {
      const auto c_balance = double{*table->get_value<float>(ColumnID{3}, row_id)};
      const auto c_ytd_payment = double{*table->get_value<float>(ColumnID{4}, row_id)};
      const auto sum_ol_amount = *table->get_value<double>(ColumnID{5}, row_id);

      Assert(floats_near(c_balance + c_ytd_payment, sum_ol_amount), "Mismatching YTD for CUSTOMER and ORDER.");
    }
  }
}

}  // namespace
