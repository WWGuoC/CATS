#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include <boost/algorithm/string.hpp>

#include "cxxopts.hpp"
#include "magic_enum.hpp"

#include "benchmark_runner.hpp"
#include "cli_config_parser.hpp"
#include "file_based_benchmark_item_runner.hpp"
#include "file_based_table_generator.hpp"
#include "hyrise.hpp"
#include "storage/constraints/constraint_utils.hpp"
#include "types.hpp"
#include "utils/assert.hpp"
#include "utils/performance_warning.hpp"
#include "utils/sqlite_add_indices.hpp"

/**
 * The Join Order Benchmark was introduced by Leis et al. "How good are query optimizers, really?". It runs on an IMDB
 * database from ~2013 that gets downloaded if necessary as part of running this benchmark. Its 113 queries are obtained
 * from the "third_party/join-order-benchmark" submodule. For an overview of the schema, see:
 * https://doi.org/10.1007/s00778-017-0480-7 (Leis et al. "Query optimization through the looking glass, and what we
 * found running the Join Order Benchmark" - 2.1 The IMDB data set, Fig. 2, p. 645.)
 */

using namespace hyrise;  // NOLINT(build/namespaces)

void add_key_constraints(std::unordered_map<std::string, BenchmarkTableInfo>& table_info_by_name) {
  // Get all tables.
  const auto& aka_name_table = table_info_by_name.at("aka_name").table;
  const auto& aka_title_table = table_info_by_name.at("aka_title").table;
  const auto& cast_info_table = table_info_by_name.at("cast_info").table;
  const auto& char_name_table = table_info_by_name.at("char_name").table;
  const auto& comp_cast_type_table = table_info_by_name.at("comp_cast_type").table;
  const auto& company_name_table = table_info_by_name.at("company_name").table;
  const auto& company_type_table = table_info_by_name.at("company_type").table;
  const auto& complete_cast_table = table_info_by_name.at("complete_cast").table;
  const auto& info_type_table = table_info_by_name.at("info_type").table;
  const auto& keyword_table = table_info_by_name.at("keyword").table;
  const auto& kind_type_table = table_info_by_name.at("kind_type").table;
  const auto& link_type_table = table_info_by_name.at("link_type").table;
  const auto& movie_companies_table = table_info_by_name.at("movie_companies").table;
  const auto& movie_info_table = table_info_by_name.at("movie_info").table;
  const auto& movie_info_idx_table = table_info_by_name.at("movie_info_idx").table;
  const auto& movie_keyword_table = table_info_by_name.at("movie_keyword").table;
  const auto& movie_link_table = table_info_by_name.at("movie_link").table;
  const auto& name_table = table_info_by_name.at("name").table;
  const auto& person_info_table = table_info_by_name.at("person_info").table;
  const auto& role_type_table = table_info_by_name.at("role_type").table;
  const auto& title_table = table_info_by_name.at("title").table;

  // Set constraints.

  // aka_name - 1 PK, 1 FK.
  primary_key_constraint(aka_name_table, {"id"});
  foreign_key_constraint(aka_name_table, {"person_id"}, name_table, {"id"});

  // aka_title - 1 PK, 1 FK.
  primary_key_constraint(aka_title_table, {"id"});
  foreign_key_constraint(aka_title_table, {"movie_id"}, title_table, {"id"});

  // cast_info - 1 PK, 4 FKs.
  primary_key_constraint(cast_info_table, {"id"});
  foreign_key_constraint(cast_info_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(cast_info_table, {"person_id"}, name_table, {"id"});
  foreign_key_constraint(cast_info_table, {"person_role_id"}, char_name_table, {"id"});
  foreign_key_constraint(cast_info_table, {"role_id"}, role_type_table, {"id"});

  // char_name - 1 PK.
  primary_key_constraint(char_name_table, {"id"});

  // comp_cast_type - 1 PK.
  primary_key_constraint(comp_cast_type_table, {"id"});

  // company_name - 1 PK.
  primary_key_constraint(company_name_table, {"id"});

  // company_type - 1 PK.
  primary_key_constraint(company_type_table, {"id"});

  // complete_cast - 1 PK, 3 FKs.
  primary_key_constraint(complete_cast_table, {"id"});
  foreign_key_constraint(complete_cast_table, {"subject_id"}, comp_cast_type_table, {"id"});
  foreign_key_constraint(complete_cast_table, {"status_id"}, comp_cast_type_table, {"id"});
  foreign_key_constraint(complete_cast_table, {"movie_id"}, title_table, {"id"});

  // info_type - 1 PK.
  primary_key_constraint(info_type_table, {"id"});

  // keyword - 1 PK.
  primary_key_constraint(keyword_table, {"id"});

  // kind_type - 1 PK.
  primary_key_constraint(kind_type_table, {"id"});

  // link_type - 1 PK.
  primary_key_constraint(link_type_table, {"id"});

  // movie_companies - 1 PK, 3 FKs.
  primary_key_constraint(movie_companies_table, {"id"});
  foreign_key_constraint(movie_companies_table, {"company_id"}, company_name_table, {"id"});
  foreign_key_constraint(movie_companies_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_companies_table, {"company_type_id"}, company_type_table, {"id"});

  // movie_info - 1 PK, 2 FKs.
  primary_key_constraint(movie_info_table, {"id"});
  foreign_key_constraint(movie_info_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_info_table, {"info_type_id"}, info_type_table, {"id"});

  // movie_info_idx - 1 PK, 2 FKs.
  primary_key_constraint(movie_info_idx_table, {"id"});
  foreign_key_constraint(movie_info_idx_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_info_idx_table, {"info_type_id"}, info_type_table, {"id"});

  // movie_keyword - 1 PK, 2 FKs.
  primary_key_constraint(movie_keyword_table, {"id"});
  foreign_key_constraint(movie_keyword_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_keyword_table, {"keyword_id"}, keyword_table, {"id"});

  // movie_link - 1 PK, 3 FKs.
  primary_key_constraint(movie_link_table, {"id"});
  foreign_key_constraint(movie_link_table, {"movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_link_table, {"linked_movie_id"}, title_table, {"id"});
  foreign_key_constraint(movie_link_table, {"link_type_id"}, link_type_table, {"id"});

  // name - 1 PK.
  primary_key_constraint(name_table, {"id"});

  // person_info - 1 PK, 2 FKs.
  primary_key_constraint(person_info_table, {"id"});
  foreign_key_constraint(person_info_table, {"person_id"}, name_table, {"id"});
  foreign_key_constraint(person_info_table, {"info_type_id"}, info_type_table, {"id"});

  // role_type - 1 PK.
  primary_key_constraint(role_type_table, {"id"});

  // title - 1 PK, 1 FK.
  primary_key_constraint(title_table, {"id"});
  foreign_key_constraint(title_table, {"kind_id"}, kind_type_table, {"id"});
}

int main(int argc, char* argv[]) {
  auto cli_options = BenchmarkRunner::get_basic_cli_options("Hyrise Join Order Benchmark");

  const auto DEFAULT_TABLE_PATH = "imdb_data";
  const auto DEFAULT_QUERY_PATH = "third_party/join-order-benchmark";

  // clang-format off
  cli_options.add_options()
  ("table_path", "Directory containing the Tables as csv, tbl or binary files. CSV files require meta-files, see csv_meta.hpp or any *.csv.json file.", cxxopts::value<std::string>()->default_value(DEFAULT_TABLE_PATH)) // NOLINT
  ("query_path", "Directory containing the .sql files of the Join Order Benchmark", cxxopts::value<std::string>()->default_value(DEFAULT_QUERY_PATH)) // NOLINT
  ("q,queries", "Subset of queries to run as a comma separated list", cxxopts::value<std::string>()->default_value("all")) // NOLINT
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
  ("placement_kappa", "[IMPL-4] Cardinality-estimate perturbation factor kappa (multiplied into every "
                      "optimizer cardinality estimate the Plan Analyzer reads); E9B arm; 1.0 = identity",
                      cxxopts::value<float>()->default_value("1.0")) // NOLINT
  ("placement_trace", "[EDBT v4 B1] Observation-only placement trace (default off). Writes "
                      "<json>_decision_trace.csv + <json>_operator_segment_trace.csv; run identity "
                      "from EDBT_V4_RUN_ID / EDBT_V4_REP env vars",
                      cxxopts::value<bool>()->default_value("false")) // NOLINT
  ("placement_trace_measured", "[EDBT v4 B2] Single-client measured diagnostic pass (requires "
                              "--placement_trace and --clients 1): sequential per-query counter-"
                              "snapshot execution after the initial placement; measured_per_execution "
                              "rows in the operator trace + <json>_measured_segment_access.csv",
                              cxxopts::value<bool>()->default_value("false")) // NOLINT
      ("weisgut_static_column", "[Phase W] Weisgut-style static whole-column placement baseline "
                              "(frozen column profile via --column_profile_path; one-time packing, "
                              "no daemon, no replicas)", cxxopts::value<bool>()->default_value("false")) // NOLINT
    ("column_profile_path", "[Phase W] frozen column_profile.csv for --weisgut_static_column",
     cxxopts::value<std::string>()->default_value("")) // NOLINT
    ("column_profile_out", "[Phase W] profile mode: dump column_profile.csv after the run "
                           "(observe-only, no placement)", cxxopts::value<std::string>()->default_value("")) // NOLINT
("cost_c3_greedy", "[EDBT v4 B3] Cost-C3 greedy baseline: priority = predicted cost difference "
                     "cost(CXL) - cost(DRAM) from the calibration table and the raw four-pattern "
                     "segment access counters (sequential/monotonic/random/point); shares the "
                     "strict-fit mapper and byte budget; requires --c3_calibration; mutually "
                     "exclusive with --ablate_d1 and --placement_wfreq",
                      cxxopts::value<bool>()->default_value("false")) // NOLINT
  ("c3_calibration", "[EDBT v4 B3] C3 calibration CSV path "
                     "(device,gamma,data_type,bytes_per_value,ns_per_access_per_byte)",
                      cxxopts::value<std::string>()->default_value("")); // NOLINT
  // clang-format on

  auto benchmark_config = std::shared_ptr<BenchmarkConfig>{};
  auto query_path = std::string{};
  auto table_path = std::string{};
  // Comma-separated query names or "all"
  auto queries_str = std::string{};

  // CXL / semantic placement options
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
  auto placement_kappa = 1.0f;
  auto placement_trace = false;
  auto placement_trace_measured = false;
  auto cost_c3_greedy = false;
  auto weisgut_static_column = false;
  auto column_profile_path = std::string{};
  auto column_profile_out = std::string{};
  auto c3_calibration = std::string{};

  // Parse command line args
  const auto cli_parse_result = cli_options.parse(argc, argv);

  if (CLIConfigParser::print_help_if_requested(cli_options, cli_parse_result)) {
    return 0;
  }

  query_path = cli_parse_result["query_path"].as<std::string>();
  table_path = cli_parse_result["table_path"].as<std::string>();
  queries_str = cli_parse_result["queries"].as<std::string>();

  benchmark_config = CLIConfigParser::parse_cli_options(cli_parse_result);

  // Parse CXL / semantic placement options
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
  placement_kappa = cli_parse_result["placement_kappa"].as<float>();
  placement_trace = cli_parse_result["placement_trace"].as<bool>();
  placement_trace_measured = cli_parse_result["placement_trace_measured"].as<bool>();
  cost_c3_greedy = cli_parse_result["cost_c3_greedy"].as<bool>();
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
  c3_calibration = cli_parse_result["c3_calibration"].as<std::string>();

  // [EDBT v4 B2] the measured diagnostic pass is only meaningful with sequential
  // execution: a shared counter delta under concurrency is not a per-execution
  // measurement. Gate it hard instead of fabricating attribution.
  if (placement_trace_measured) {
    if (!placement_trace) {
      std::cerr << "--placement_trace_measured requires --placement_trace\n";
      return 1;
    }
    if (benchmark_config->clients != 1) {
      std::cerr << "--placement_trace_measured requires --clients 1 (got " << benchmark_config->clients << ")\n";
      return 1;
    }
  }

  // [EDBT v4 B3] cost_c3_greedy is a third migration policy, not an ablation
  // arm: it must not be combined with the d1/wfreq priority variants and needs
  // its calibration table.
  if (cost_c3_greedy) {
    if (ablate_d1 || placement_wfreq) {
      std::cerr << "--cost_c3_greedy is mutually exclusive with --ablate_d1 and --placement_wfreq\n";
      return 1;
    }
    if (c3_calibration.empty()) {
      std::cerr << "--cost_c3_greedy requires --c3_calibration <csv>\n";
      return 1;
    }
  }

  // Check that the options "query_path" and "table_path" were specified
  if (query_path.empty() || table_path.empty()) {
    std::cerr << "Need to specify --query_path=path/to/queries and --table_path=path/to/table_files\n";
    std::cerr << cli_options.help({}) << '\n';
    return 1;
  }

  /**
   * Use a Python script to download and unzip the IMDB. We do this in Python and not in C++ because downloading and
   * unzipping is straight forward in Python (and we suspect in C++ it might be... cumbersome).
   */
  const auto setup_imdb_command = "python3 scripts/setup_imdb.py " + table_path;
  const auto setup_imdb_return_code = std::system(setup_imdb_command.c_str());
  Assert(setup_imdb_return_code == 0, "setup_imdb.py failed. Did you run the benchmark from the project root dir?");

  // The join-order-benchmark ships with these two .sql scripts, but we do not want to run them as part of the benchmark
  // as they do not contains actual queries
  const auto non_query_file_names = std::unordered_set<std::string>{"fkindexes.sql", "schema.sql"};

  std::cout << "- Benchmarking queries from " << query_path << '\n';
  std::cout << "- Running on tables from " << table_path << '\n';

  auto query_subset = std::optional<std::unordered_set<std::string>>{};
  if (queries_str == "all") {
    std::cout << "- Running all queries from specified path\n";
  } else {
    std::cout << "- Running subset of queries: " << queries_str << '\n';

    // "a, b, c, d" -> ["a", " b", " c", " d"]
    auto query_subset_untrimmed = std::vector<std::string>{};
    boost::algorithm::split(query_subset_untrimmed, queries_str, boost::is_any_of(","));

    // ["a", " b", " c", " d"] -> ["a", "b", "c", "d"]
    query_subset.emplace();
    for (const auto& query_name : query_subset_untrimmed) {
      query_subset->emplace(boost::trim_copy(query_name));
    }
  }

  // Run the benchmark
  // Build list of local memory NUMA node IDs
  auto lmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_lmem_ids.empty()) {
    auto local_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_lmem_ids, boost::is_any_of(","));
    boost::split(local_numa_nodes_str, comma_separated_lmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(local_numa_nodes_str.begin(), local_numa_nodes_str.end(), std::back_inserter(lmem_node_ids), [](const auto& node_id_str) {
      return static_cast<NumaNodeID>(std::stoul(node_id_str));
    });
  }

  // Build list of remote memory NUMA node IDs
  auto rmem_node_ids = NumaNodeIDs{};
  if (!comma_separated_rmem_ids.empty()) {
    auto remote_numa_nodes_str = std::vector<std::string>();
    boost::trim_if(comma_separated_rmem_ids, boost::is_any_of(","));
    boost::split(remote_numa_nodes_str, comma_separated_rmem_ids, boost::is_any_of(","), boost::token_compress_on);
    std::transform(remote_numa_nodes_str.begin(), remote_numa_nodes_str.end(), std::back_inserter(rmem_node_ids), [](const auto& node_id_str) {
      return static_cast<NumaNodeID>(std::stoul(node_id_str));
    });
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

  // Print placement configuration
  std::cout << "- Local memory NUMA node IDs: [ " << comma_separated_lmem_ids << " ]\n";
  if (!comma_separated_rmem_ids.empty()) {
    std::cout << "- Remote memory NUMA node IDs: [ " << comma_separated_rmem_ids << " ]\n";
  }
  if (semantic) {
    std::cout << "- Semantic placement: ON (ablate d1=" << ablate_d1 << ", d2=" << ablate_d2
              << ", d3_replica=" << ablate_d3_replica << ", d3_gc=" << ablate_d3_gc << ")\n";
    if (dram_first) {
      std::cout << "- DRAM-first mode: ON (demote_pct=" << demote_pct
                << ", daemon_period=" << daemon_period << "s)\n";
    }
  }

  // Determine placement type
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

  // JOB does not have a scale factor; use 1.0 as placeholder
  constexpr auto job_scale_factor = 1.0f;
  const auto json_path = benchmark_config->output_file_path.value_or("");

  Hyrise::get().placement_options = {.scale_factor = job_scale_factor, .lmem_node_ids = lmem_node_ids,
                                     .rmem_node_ids = rmem_node_ids, .rmem_weights = rmem_weights, .type = type,
                                     .num_most_frequently_columns_local = fixed_local_columns,
                                     .json_path = json_path,
                                     .ablate_d1 = ablate_d1, .ablate_d2 = ablate_d2,
                                     .ablate_d3_replica = ablate_d3_replica, .ablate_d3_gc = ablate_d3_gc,
                                     .dram_first = dram_first, .daemon_period_seconds = daemon_period,
                                     .demote_pct = demote_pct,
                                     .dram_capacity_gb = dram_capacity_gb,
                                     .cxl_capacity_gb = cxl_capacity_gb,
                                     .placement_freeze_at_timed_start = placement_freeze,
                                     .placement_wfreq = placement_wfreq,
                                     .placement_kappa = placement_kappa,
                                     .placement_query_path = query_path,
                                     .placement_trace = placement_trace,
                                     .placement_trace_measured = placement_trace_measured,
                                     .cost_c3_greedy = cost_c3_greedy,
                                     .c3_calibration_path = c3_calibration,
                                     .weisgut_static_column = weisgut_static_column,
                                     .column_profile_path = column_profile_path,
                                     .column_profile_out = column_profile_out};

  auto context = BenchmarkRunner::create_context(*benchmark_config);
  auto table_generator = std::make_unique<FileBasedTableGenerator>(benchmark_config, table_path);
  table_generator->set_add_constraints_callback(add_key_constraints);
  auto benchmark_item_runner =
      std::make_unique<FileBasedBenchmarkItemRunner>(benchmark_config, query_path, non_query_file_names, query_subset);

  if (benchmark_item_runner->items().empty()) {
    std::cout << "No items to run.\n";
    return 1;
  }

  auto benchmark_runner = std::make_shared<BenchmarkRunner>(*benchmark_config, std::move(benchmark_item_runner),
                                                            std::move(table_generator), context);
  Hyrise::get().benchmark_runner = benchmark_runner;

  if (benchmark_config->verify) {
    add_indices_to_sqlite(query_path + "/schema.sql", query_path + "/fkindexes.sql", benchmark_runner->sqlite_wrapper);
  }

  std::cout << "done.\n";

  benchmark_runner->run();
}
