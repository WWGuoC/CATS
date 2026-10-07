#include <chrono>
#include <iostream>
#include <string>

#include <boost/algorithm/string.hpp>
#include <boost/lexical_cast.hpp>

#include "cxxopts.hpp"
#include "magic_enum.hpp"
#include "SQLParserResult.h"

#include "benchmark_runner.hpp"
#include "cli_config_parser.hpp"
#include "hyrise.hpp"
#include "jcch/jcch_benchmark_item_runner.hpp"
#include "jcch/jcch_table_generator.hpp"
#include "tpch/tpch_benchmark_item_runner.hpp"
#include "tpch/tpch_queries.hpp"
#include "tpch/tpch_table_generator.hpp"
#include "utils/assert.hpp"
#include "utils/sqlite_add_indices.hpp"

using namespace hyrise;  // NOLINT

/**
 * This benchmark measures Hyrise's performance executing the TPC-H *queries*, it doesn't (yet) support running the
 * TPC-H *benchmark* exactly as it is specified.
 * (Among other things, the TPC-H requires performing data refreshes and has strict requirements for the number of
 * sessions running in parallel. See http://www.tpc.org/tpch/default.asp for more info)
 * The benchmark offers a wide range of options (scale_factor, chunk_size, ...) but most notably it offers two modes:
 * IndividualQueries and PermutedQuerySets. See docs on BenchmarkMode for details.
 * The benchmark will stop issuing new queries if either enough iterations have taken place or enough time has passed.
 *
 * main() is mostly concerned with parsing the CLI options while BenchmarkRunner.run() performs the actual benchmark
 * logic.
 *
 * The same binary is used to run the JCC-H benchmark. For this, simply use the -j flag.
 */

int main(int argc, char* argv[]) {
  auto cli_options = BenchmarkRunner::get_basic_cli_options("TPC-H/JCC-H Benchmark");

  // clang-format off
  cli_options.add_options()
    ("s,scale", "Database scale factor (10.0 ~ 10 GB)", cxxopts::value<float>()->default_value("10"))
    ("q,queries", "Specify queries to run (comma-separated query ids, e.g. \"--queries 1,3,19\"), default is all", cxxopts::value<std::string>()) // NOLINT
    ("use_prepared_statements", "Use prepared statements instead of random SQL strings", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("j,jcch", "Use JCC-H data and query generators instead of TPC-H. If this parameter is used, table data always "
               "contains skew. With --jcch=skewed, queries are generated to be affected by this skew. With "
               "--jcch=normal, query parameters access the unskewed part of the tables ", cxxopts::value<std::string>()->default_value("")) // NOLINT
    ("clustering", "Clustering of TPC-H data. The default of --clustering=None means the data is stored as generated "
                   "by the TPC-H data generator. With --clustering=\"Pruning\", the two largest tables 'lineitem' "
                   "and 'orders' are sorted by 'l_shipdate' and 'o_orderdate' for improved chunk pruning. Both are "
                   "legal TPC-H input data.", cxxopts::value<std::string>()->default_value("None")) // NOLINT
    ("lmem_ids", "Specify the local memory NUMA nodes (comma-separated NUMA IDs, e.g., --lmem_ids 0,2)", cxxopts::value<std::string>()->default_value("0")) // NOLINT
    ("rmem_ids", "Specify the remote memory NUMA nodes (comma-separated NUMA IDs, e.g., --rmem_ids 1,3,4)", cxxopts::value<std::string>()) // NOLINT
    ("rmem_weights", "Specify the remote memory NUMA node weights (comma-separated weights, e.g., --rmem_ids 2,1,1)", cxxopts::value<std::string>()) // NOLINT
    ("fixed_columns", "Specify the number of most frequently accesses columns fixed in local memory", cxxopts::value<uint32_t>()->default_value("0")) // NOLINT
    ("semantic", "Use semantic proactive placement plugin (d1+d2+d3)", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d1", "Disable d1: use access-frequency instead of LQP plan analysis", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d2", "Disable d2: column-level migration instead of segment-level", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d3_replica", "Disable d3 replicas: single-copy migration", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("ablate_d3_gc", "Disable d3 semantic GC: LRU fallback only", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("dram_first", "DRAM-first mode: data starts on DRAM, demote cold segments to CXL", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("daemon_period", "Daemon period in seconds (0 = no daemon)", cxxopts::value<uint32_t>()->default_value("5")) // NOLINT
    ("demote_pct", "Fraction of cold segments to demote to CXL in pre-placement (0-1)", cxxopts::value<float>()->default_value("0.3")) // NOLINT
    ("dram_capacity_gb", "DRAM buffer size in GB (0 = auto, derived from scale factor)", cxxopts::value<float>()->default_value("0")) // NOLINT
    ("cxl_capacity_gb", "CXL buffer size in GB (0 = auto, derived from scale factor)", cxxopts::value<float>()->default_value("0")) // NOLINT
    ("placement_freeze", "[IMPL-1] Freeze placement at timed-phase start: warmup places (budget-clamped), "
                         "timed phase is static; daemon keeps sampling", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("placement_wfreq", "[IMPL-2] Cost-weighted frequency priority: freq_w = cost(atype) x access_rate "
                        "(cost = semantic score table); E8 arm", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("weisgut_static_column", "[Phase W] Weisgut-style static whole-column placement baseline "
                              "(frozen column profile via --column_profile_path; one-time packing, "
                              "no daemon, no replicas)", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("column_profile_path", "[Phase W] frozen column_profile.csv for --weisgut_static_column",
     cxxopts::value<std::string>()->default_value("")) // NOLINT
    ("column_profile_out", "[Phase W] profile mode: dump column_profile.csv after the run "
                           "(observe-only, no placement)", cxxopts::value<std::string>()->default_value("")) // NOLINT
    ("cost_c3_greedy", "[EDBT v4 B3] Cost-C3 greedy baseline: typed-cost benefit priority "
                       "(launcher wiring for the frozen plugin feature; mirrors the JOB flag)",
     cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("c3_calibration", "Calibration CSV for --cost_c3_greedy (mirrors the JOB flag)",
     cxxopts::value<std::string>()->default_value("")) // NOLINT
    ("phase1_seconds", "E4: seconds of timed phase 1 before the item pool switches to --phase2_queries (0 = off, "
                       "Shuffled mode only)", cxxopts::value<uint32_t>()->default_value("0")) // NOLINT
    ("phase1_queries", "E4: comma-separated query ids for timed phase 1 (empty = all selected queries), requires "
                       "--phase1_seconds", cxxopts::value<std::string>()) // NOLINT
    ("phase2_queries", "E4: comma-separated query ids for timed phase 2, requires --phase1_seconds", cxxopts::value<std::string>()); // NOLINT
  // clang-format on

  auto config = std::shared_ptr<BenchmarkConfig>{};
  auto comma_separated_queries = std::string{};
  auto scale_factor = float{};
  auto use_prepared_statements = false;
  auto jcch = false;
  auto jcch_skewed = false;
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
  auto placement_freeze = false;
  auto placement_wfreq = false;
  auto cost_c3_greedy = false;
  auto c3_calibration = std::string{};
  auto weisgut_static_column = false;
  auto column_profile_path = std::string{};
  auto column_profile_out = std::string{};
  auto phase1_seconds = uint32_t{0};
  auto comma_separated_phase1_queries = std::string{};
  auto comma_separated_phase2_queries = std::string{};

  // Parse command line args
  const auto cli_parse_result = cli_options.parse(argc, argv);

  if (CLIConfigParser::print_help_if_requested(cli_options, cli_parse_result)) {
    return 0;
  }

  if (cli_parse_result.count("queries")) {
    comma_separated_queries = cli_parse_result["queries"].as<std::string>();
  }

  scale_factor = cli_parse_result["scale"].as<float>();

  config = CLIConfigParser::parse_cli_options(cli_parse_result);
  {
    const auto json_ext = std::string{".json"};
    const auto& path = config->output_file_path;
    Assert(path, "Output file path is not set (-o <path>).");
    Assert((*path).size() >= json_ext.size() && (*path).substr((*path).size() - json_ext.size()) == json_ext,
      "Output file path does not end with .json");
  }

  use_prepared_statements = cli_parse_result["use_prepared_statements"].as<bool>();
  jcch = cli_parse_result.count("jcch");
  if (jcch) {
    const auto jcch_mode = cli_parse_result["jcch"].as<std::string>();
    if (jcch_mode == "skewed") {
      jcch_skewed = true;
    } else if (jcch_mode == "normal") {  // NOLINT
      jcch_skewed = false;
    } else {
      Fail("Invalid JCC-H mode, use skewed or normal.");
    }
  }

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
  placement_freeze = cli_parse_result["placement_freeze"].as<bool>();
  placement_wfreq = cli_parse_result["placement_wfreq"].as<bool>();
  cost_c3_greedy = cli_parse_result["cost_c3_greedy"].as<bool>();
  c3_calibration = cli_parse_result["c3_calibration"].as<std::string>();
  weisgut_static_column = cli_parse_result["weisgut_static_column"].as<bool>();
  column_profile_path = cli_parse_result["column_profile_path"].as<std::string>();
  column_profile_out = cli_parse_result["column_profile_out"].as<std::string>();
  if (weisgut_static_column && column_profile_path.empty()) {
    std::cerr << "--weisgut_static_column requires --column_profile_path <frozen.csv>\n";
    return 1;
  }
  if (weisgut_static_column && !column_profile_out.empty()) {
    std::cerr << "--weisgut_static_column (placement) and --column_profile_out (profiling) are mutually exclusive\n";
    return 1;
  }
  // [EDBT v4 B3] mirror the JOB mutual exclusions: Cost-C3 is a third policy,
  // not an ablation arm, and requires its calibration table.
  if (cost_c3_greedy && (ablate_d1 || placement_wfreq)) {
    std::cerr << "--cost_c3_greedy is mutually exclusive with --ablate_d1 and --placement_wfreq\n";
    return 1;
  }
  if (cost_c3_greedy && c3_calibration.empty()) {
    std::cerr << "--cost_c3_greedy requires --c3_calibration <csv>\n";
    return 1;
  }
  phase1_seconds = cli_parse_result["phase1_seconds"].as<uint32_t>();
  if (cli_parse_result.count("phase1_queries")) {
    comma_separated_phase1_queries = cli_parse_result["phase1_queries"].as<std::string>();
  }
  if (cli_parse_result.count("phase2_queries")) {
    comma_separated_phase2_queries = cli_parse_result["phase2_queries"].as<std::string>();
  }

  auto clustering_configuration = ClusteringConfiguration::None;
  if (cli_parse_result.count("clustering")) {
    auto clustering_configuration_parameter = cli_parse_result["clustering"].as<std::string>();
    if (clustering_configuration_parameter == "Pruning") {
      clustering_configuration = ClusteringConfiguration::Pruning;
    } else if (clustering_configuration_parameter != "None") {
      Fail("Invalid clustering config: '" + clustering_configuration_parameter + "'.");
    }

    std::cout << "- Clustering with '" << magic_enum::enum_name(clustering_configuration) << "' configuration\n";
  }

  std::vector<BenchmarkItemID> item_ids;

  // Build list of query ids to be benchmarked and display it
  if (comma_separated_queries.empty()) {
    std::transform(tpch_queries.begin(), tpch_queries.end(), std::back_inserter(item_ids), [](auto& pair) {
      return BenchmarkItemID{pair.first - 1};
    });
  } else {
    // Split the input into query ids, ignoring leading, trailing, or duplicate commas
    auto item_ids_str = std::vector<std::string>();
    boost::trim_if(comma_separated_queries, boost::is_any_of(","));
    boost::split(item_ids_str, comma_separated_queries, boost::is_any_of(","), boost::token_compress_on);
    std::transform(item_ids_str.begin(), item_ids_str.end(), std::back_inserter(item_ids), [](const auto& item_id_str) {
      const auto item_id =
          BenchmarkItemID{boost::lexical_cast<BenchmarkItemID::base_type, std::string>(item_id_str) - 1};
      DebugAssert(item_id < 22, "There are only 22 queries.");
      return item_id;
    });
  }

  // ── E4 (plan v2 §5): workload-shift phase switching ─────────────────────────
  // --phase1_seconds and --phase2_queries together enable a two-phase timed run
  // (Shuffled mode only): the timed pool starts at --phase1_queries (empty = all
  // selected queries) and switches to --phase2_queries afterwards. Query ids use
  // the same 1-based numbering as --queries.
  if (phase1_seconds > 0 || !comma_separated_phase1_queries.empty() || !comma_separated_phase2_queries.empty()) {
    Assert(phase1_seconds > 0 && !comma_separated_phase2_queries.empty(),
           "E4 phase switching requires both --phase1_seconds and --phase2_queries");
    Assert(config->benchmark_mode == BenchmarkMode::Shuffled,
           "E4 phase switching requires Shuffled mode (-m shuffled)");

    const auto parse_query_ids = [](const auto& comma_separated) {
      auto query_ids = std::vector<size_t>{};
      auto query_ids_str = std::vector<std::string>{};
      auto trimmed = comma_separated;
      boost::trim_if(trimmed, boost::is_any_of(","));
      boost::split(query_ids_str, trimmed, boost::is_any_of(","), boost::token_compress_on);
      std::transform(query_ids_str.begin(), query_ids_str.end(), std::back_inserter(query_ids),
                     [](const auto& query_id_str) {
                       const auto query_id = static_cast<size_t>(
                           boost::lexical_cast<BenchmarkItemID::base_type, std::string>(query_id_str));
                       DebugAssert(query_id >= 1 && query_id <= 22, "There are only 22 queries.");
                       return query_id - 1;
                     });
      return query_ids;
    };

    config->phase1_duration = std::chrono::seconds{phase1_seconds};
    if (!comma_separated_phase1_queries.empty()) {
      config->phase1_item_ids = parse_query_ids(comma_separated_phase1_queries);
    }
    config->phase2_item_ids = parse_query_ids(comma_separated_phase2_queries);
    std::cout << "- [E4] Phase switching enabled: phase 1 for " << phase1_seconds << "s ("
              << (config->phase1_item_ids.empty() ? std::string{"all selected"} : std::to_string(config->phase1_item_ids.size()))
              << " queries), then " << config->phase2_item_ids.size() << " phase-2 queries\n";
  }

  // Build list of memory ids
  auto lmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_lmem_ids.empty()) {
    auto local_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_lmem_ids, boost::is_any_of(","));
    boost::split(local_numa_nodes_str, comma_separated_lmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(local_numa_nodes_str.begin(), local_numa_nodes_str.end(), std::back_inserter(lmem_node_ids), [](const auto& node_id_str) {
      return static_cast<NumaNodeID>(std::stoul(node_id_str));
    });
  }

  auto rmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_rmem_ids.empty()) {
    auto remote_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_rmem_ids, boost::is_any_of(","));
    boost::split(remote_numa_nodes_str, comma_separated_rmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(remote_numa_nodes_str.begin(), remote_numa_nodes_str.end(), std::back_inserter(rmem_node_ids), [](const auto& node_id_str) {
      return static_cast<NumaNodeID>(std::stoul(node_id_str));
    });
  }

  {
    auto print_lmem_node_ids = std::vector<std::string>();
    std::for_each(lmem_node_ids.begin(), lmem_node_ids.end(), [&print_lmem_node_ids](auto& id) {
      print_lmem_node_ids.push_back(std::to_string(id));
    });
    std::cout << "- Local memory NUMA node IDs: [ " << boost::algorithm::join(print_lmem_node_ids, ", ") << " ]\n";
  }

  {
    auto print_rmem_node_ids = std::vector<std::string>();
    std::for_each(rmem_node_ids.begin(), rmem_node_ids.end(), [&print_rmem_node_ids](auto& id) {
      print_rmem_node_ids.push_back(std::to_string(id));
    });
    std::cout << "- Remote memory NUMA node IDs: [ " << boost::algorithm::join(print_rmem_node_ids, ", ") << " ]\n";
  }


  // Build list of remote memory weights
  auto rmem_weights = InterleavingWeights{};
  if (!comma_separated_rmem_weights.empty()) {
    auto rmem_weights_str = std::vector<std::string>();
    boost::trim_if(comma_separated_rmem_weights, boost::is_any_of(","));
    boost::split(rmem_weights_str, comma_separated_rmem_weights, boost::is_any_of(","), boost::token_compress_on);
    std::transform(rmem_weights_str.begin(), rmem_weights_str.end(), std::back_inserter(rmem_weights), [](const auto& weight_str) {
      return static_cast<uint32_t>(std::stoul(weight_str));
    });
  }

  {
    auto print_rmem_weights = std::vector<std::string>();
    std::for_each(rmem_weights.begin(), rmem_weights.end(), [&print_rmem_weights](auto& weight) {
      print_rmem_weights.push_back(std::to_string(weight));
    });
    std::cout << "- Remote memory NUMA node weights: [ " << boost::algorithm::join(print_rmem_weights, ", ") << " ]\n";
  }

  std::cout << "- # columns fixed in local memory: " << fixed_local_columns << "\n";
  if (semantic) {
    std::cout << "- Semantic placement: ON (ablate d1=" << ablate_d1 << ", d2=" << ablate_d2
              << ", d3_replica=" << ablate_d3_replica << ", d3_gc=" << ablate_d3_gc << ")\n";
    if (dram_first) {
      std::cout << "- DRAM-first mode: ON (demote_pct=" << demote_pct
                << ", daemon_period=" << daemon_period << "s)\n";
    }
  }

  const auto type = [&](){
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
  Hyrise::get().placement_options = {.scale_factor = scale_factor, .lmem_node_ids = lmem_node_ids,
                                     .rmem_node_ids = rmem_node_ids, .rmem_weights = rmem_weights, .type = type,
                                     .num_most_frequently_columns_local = fixed_local_columns,
                                     .json_path = *config->output_file_path,
                                     .ablate_d1 = ablate_d1, .ablate_d2 = ablate_d2,
                                     .ablate_d3_replica = ablate_d3_replica, .ablate_d3_gc = ablate_d3_gc,
                                     .dram_first = dram_first, .daemon_period_seconds = daemon_period,
                                     .demote_pct = demote_pct,
                                     .dram_capacity_gb = dram_capacity_gb,
                                     .cxl_capacity_gb = cxl_capacity_gb,
                                     .placement_freeze_at_timed_start = placement_freeze,
                                     .placement_wfreq = placement_wfreq,
                                     .cost_c3_greedy = cost_c3_greedy,
                                     .c3_calibration_path = c3_calibration,
                                     .weisgut_static_column = weisgut_static_column,
                                     .column_profile_path = column_profile_path,
                                     .column_profile_out = column_profile_out};

  std::cout << "- Benchmarking Queries: [ ";
  auto printable_item_ids = std::vector<std::string>();
  std::for_each(item_ids.begin(), item_ids.end(), [&printable_item_ids](auto& id) {
    printable_item_ids.push_back(std::to_string(id + 1));
  });
  std::cout << boost::algorithm::join(printable_item_ids, ", ") << " ]\n";

  auto context = BenchmarkRunner::create_context(*config);

  Assert(!use_prepared_statements || !config->verify, "SQLite validation does not work with prepared statements.");

  if (config->verify) {
    // Hack: We cannot verify Q15, thus we remove it from the list of queries
    auto it = std::remove(item_ids.begin(), item_ids.end(), 15 - 1);
    if (it != item_ids.end()) {
      // The problem is that the last part of the query, "DROP VIEW", does not return a table. Since we also have
      // the TPC-H test against a known-to-be-good table, we do not want the additional complexity for handling this
      // in the BenchmarkRunner.
      std::cout << "- Skipping Query 15 because it cannot easily be verified\n";
      item_ids.erase(it, item_ids.end());
    }
  }

  std::cout << "- " << (jcch ? "JCC-H" : "TPC-H") << " scale factor is " << scale_factor << '\n';
  std::cout << "- Using prepared statements: " << (use_prepared_statements ? "yes" : "no") << '\n';

  // Add TPCH-specific information
  context.emplace("scale_factor", scale_factor);
  context.emplace("clustering", magic_enum::enum_name(clustering_configuration));
  context.emplace("use_prepared_statements", use_prepared_statements);

  auto table_generator = std::unique_ptr<AbstractTableGenerator>{};
  auto item_runner = std::unique_ptr<AbstractBenchmarkItemRunner>{};

  if (jcch) {
    // Different from the TPC-H benchmark, where the table and query generators are immediately embedded in Hyrise, the
    // JCC-H implementation calls those generators externally. This is because we would get linking conflicts if we were
    // to include both generators. Unfortunately, this approach is somewhat slower (30s to start SF1 with TPC-H, 1m18s
    // with JCC-H).
    //
    // JCC-H has both a skewed and a "normal" (i.e., unskewed) mode. The unskewed mode is not the same as TPC-H. You can
    // find details in the JCC-H paper: https://ir.cwi.nl/pub/27429

    // Try to find dbgen/qgen binaries
    auto jcch_dbgen_path =
        std::filesystem::canonical(std::string{argv[0]}).remove_filename() / "third_party/jcch-dbgen";
    Assert(std::filesystem::exists(jcch_dbgen_path / "dbgen"),
           std::string{"JCC-H dbgen not found at "} + jcch_dbgen_path.c_str());
    Assert(std::filesystem::exists(jcch_dbgen_path / "qgen"),
           std::string{"JCC-H qgen not found at "} + jcch_dbgen_path.c_str());

    // Create the jcch_data directory (if needed) and generate the jcch_data/sf-... path
    auto jcch_data_path_str = std::ostringstream{};
    jcch_data_path_str << "jcch_data/sf-" << std::noshowpoint << scale_factor;
    std::filesystem::create_directories(jcch_data_path_str.str());
    // Success of create_directories is guaranteed by the call to fs::canonical, which fails on invalid paths:
    const auto jcch_data_path = std::filesystem::canonical(jcch_data_path_str.str());
    const auto jcch_tables_path = jcch_data_path.string() + "/tables";

    std::cout << "- Using JCC-H dbgen from " << jcch_dbgen_path << '\n';
    std::cout << "- Storing JCC-H tables and query parameters in " << jcch_data_path << '\n';
    std::cout << "- JCC-H query parameters are " << (jcch_skewed ? "skewed" : "not skewed") << '\n';

    // Create the table generator and item runner
    table_generator = std::make_unique<JCCHTableGenerator>(jcch_dbgen_path, jcch_tables_path, scale_factor,
                                                           clustering_configuration, config);
    item_runner = std::make_unique<JCCHBenchmarkItemRunner>(jcch_skewed, jcch_dbgen_path, jcch_data_path, config,
                                                            use_prepared_statements, scale_factor,
                                                            clustering_configuration, item_ids);
  } else {
    table_generator = std::make_unique<TPCHTableGenerator>(scale_factor, clustering_configuration, config);
    item_runner = std::make_unique<TPCHBenchmarkItemRunner>(config, use_prepared_statements, scale_factor,
                                                            clustering_configuration, item_ids);
  }

  auto benchmark_runner =
      std::make_shared<BenchmarkRunner>(*config, std::move(item_runner), std::move(table_generator), context);
  Hyrise::get().benchmark_runner = benchmark_runner;

  if (config->verify) {
    add_indices_to_sqlite("resources/benchmark/tpch/schema.sql", "resources/benchmark/tpch/indices.sql",
                          benchmark_runner->sqlite_wrapper);
  }

  benchmark_runner->run();
}
