#!/usr/bin/env python3
"""Create synthetic bundles and exercise all three package scripts."""

from __future__ import annotations

import csv
import json
import subprocess
import sys
import tempfile
from pathlib import Path


SCRIPT_DIR = Path(__file__).resolve().parent


def write_csv(path: Path, fields: list[str], rows: list[dict]) -> None:
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fields)
        writer.writeheader()
        writer.writerows(rows)


def make_run(root: Path, config: str, rep: int, latency: float, trace: bool) -> Path:
    run = root / config / f"rep{rep}"
    run.mkdir(parents=True)
    digest = "a" * 64
    (run / "run_config.json").write_text(json.dumps({
        "config": config, "rep": rep, "git_hash": "b" * 40,
        "binary_sha256": digest, "plugin_sha256": digest,
        "command": "synthetic-test", "query_schedule_sha256": "c" * 64,
        "trace": trace, "dram_budget_bytes": 2048,
    }), encoding="utf-8")
    (run / "run.log").write_text("synthetic\n", encoding="utf-8")
    (run / "exit_code.txt").write_text("0\n", encoding="utf-8")
    (run / "benchmark.json").write_text("{}\n", encoding="utf-8")
    write_csv(run / "summary.csv", ["config", "rep", "geomean_avg_ms"], [{"config": config, "rep": rep, "geomean_avg_ms": latency}])
    write_csv(run / "per_query.csv", ["query_id", "name", "avg_ms"], [{"query_id": 1, "name": "10a", "avg_ms": latency}])
    write_csv(run / "benchmark_semantic_placement.csv", ["table_name", "chunk_id", "column_id", "access_type", "access_frequency", "target_tier", "create_replica", "is_immutable", "data_volume"], [{"table_name": "t", "chunk_id": 0, "column_id": 0, "access_type": "point", "access_frequency": 1, "target_tier": "DRAM", "create_replica": 0, "is_immutable": 1, "data_volume": 1024}])
    write_csv(run / "daemon_metrics.csv", ["unix_ts", "cycle_id", "hit_rate_pct", "promotes", "demotes", "bytes_promoted", "bytes_demoted", "daemon_cpu_ms", "replica_bytes"], [{"unix_ts": 1, "cycle_id": 1, "hit_rate_pct": 50, "promotes": 1, "demotes": 0, "bytes_promoted": 1024, "bytes_demoted": 0, "daemon_cpu_ms": 1, "replica_bytes": 0}])
    if trace:
        decision = {
            "run_id": f"{config}-{rep}", "config": config, "rep": rep,
            "unix_ts_ns": 1, "cycle_id": 1, "phase": "timed_second_half",
            "table_name": "t", "chunk_id": 0, "column_id": 0,
            "segment_bytes": 1024, "source_tier": "CXL", "target_tier": "DRAM",
            "action": "promote", "selected": "true", "rank": 1, "fits_remaining": "true",
            "policy": "cost_c3_greedy" if config == "cost_c3_greedy" else ("our_full" if config == "our_full" else "our_no_plan"),
            "data_type": "int32", "bytes_per_value": 4, "total_score": 1,
            "access_rate_raw": 1, "max_rate": 1, "norm_freq": 1,
            "access_type": "Scan", "atype_score": 0.5,
            "data_volume_bytes": 1024, "max_volume_bytes": 1024, "norm_volume": 0,
            "is_immutable": "true", "immutable_score": 1,
            "weight_atype": 0.5, "weight_freq": 0.25, "weight_volume": 0.15,
            "weight_immutable": 0.10, "term_atype": 0.25, "term_freq": 0.25,
            "term_volume": 0, "term_immutable": 0.10,
            "estimated_cardinality": 1,
            "operator_mask": "scan", "remaining_budget_before": 2048,
            "remaining_budget_after": 1024, "sequential_count": 1,
            "monotonic_count": 0, "random_count": 0, "point_count": 0,
            "dictionary_count": 0, "dictionary_handling": "excluded_reported",
            "cost_dram": 1, "cost_cxl": 2,
        }
        if config != "cost_c3_greedy":
            decision["total_score"] = 0.60
        write_csv(run / "decision_trace.csv", list(decision), [decision])
        operator = {
            "run_id": f"{config}-{rep}", "config": config, "rep": rep,
            "query_name": "10a", "execution_id": 1, "operator_id": 1,
            "operator_type": "TableScan", "table_name": "t", "chunk_id": 0,
            "column_id": 0, "estimated_rows": 10, "rows_in": 10, "rows_out": 1,
            "operator_wall_ns": 100, "actual_reads": 10, "tier_at_execution": "DRAM",
            "attribution_level": "measured_per_execution", "dram_accesses": 10, "cxl_accesses": 0,
        }
        write_csv(run / "operator_segment_trace.csv", list(operator), [operator])
    return run


def call(*args: str) -> None:
    subprocess.run([sys.executable, *args], check=True, stdout=subprocess.DEVNULL)


def main() -> int:
    with tempfile.TemporaryDirectory(prefix="edbt-pack-self-test-") as temp:
        root = Path(temp)
        core = root / "E12"
        factors = {"our_full": 0.8, "our_freq": 1.0, "cost_c3_greedy": 1.1, "dram_only": 0.7, "cxl_only": 1.5}
        for config, factor in factors.items():
            for rep in range(1, 11):
                run = make_run(core, config, rep, (100 + rep) * factor, trace=False)
                call(str(SCRIPT_DIR / "validate_run_bundle.py"), str(run), "--create-stamp")
        call(str(SCRIPT_DIR / "analyze_core_repetitions.py"), "--root", str(core), "--output-dir", str(root / "core_analysis"))

        trace_campaign = root / "trace_campaign"
        mechanism = trace_campaign / "E13"
        for config, factor in {"our_full": 0.8, "our_freq": 1.0, "cost_c3_greedy": 1.1}.items():
            for rep in range(1, 3):
                run = make_run(mechanism, config, rep, (100 + rep) * factor, trace=True)
                call(str(SCRIPT_DIR / "validate_run_bundle.py"), str(run), "--require-trace", "--budget-bytes", "2048", "--create-stamp")
        call(str(SCRIPT_DIR / "analyze_mechanism.py"), "--root", str(mechanism), "--output-dir", str(root / "mechanism_analysis"))
        overhead = trace_campaign / "E13_OVERHEAD"
        for rep in range(1, 4):
            run = make_run(overhead, "our_full", rep, (100 + rep) * 0.8 / 1.01, trace=False)
            call(str(SCRIPT_DIR / "validate_run_bundle.py"), str(run), "--create-stamp")
        # Add the third traced pair; the first two already exist under E13.
        run = make_run(mechanism, "our_full", 3, 103 * 0.8, trace=True)
        call(str(SCRIPT_DIR / "validate_run_bundle.py"), str(run), "--require-trace", "--budget-bytes", "2048", "--create-stamp")
        call(str(SCRIPT_DIR / "analyze_trace_overhead.py"), "--root", str(trace_campaign), "--output", str(root / "trace_overhead.json"))

        statistics = json.loads((root / "core_analysis" / "core_statistics.json").read_text(encoding="utf-8"))
        assert statistics["validated_runs"] == 50
        assert len(statistics["comparisons"]) == 4
        mechanism_summary = json.loads((root / "mechanism_analysis" / "mechanism_summary.json").read_text(encoding="utf-8"))
        assert mechanism_summary["validated_trace_runs"] == 6
        overhead_summary = json.loads((root / "trace_overhead.json").read_text(encoding="utf-8"))
        assert overhead_summary["gate"] == "PASS"
    print("self-test passed: validator, paired statistics, and mechanism analysis")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
