#!/usr/bin/env python3
"""Validate one EDBT experiment run before it enters the analysis set."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import sys
from pathlib import Path


BASE_FILES = (
    "run_config.json",
    "run.log",
    "exit_code.txt",
    "benchmark.json",
    "summary.csv",
    "per_query.csv",
)
MIGRATION_FILES = ("benchmark_semantic_placement.csv", "daemon_metrics.csv")
REFERENCE_CONFIGS = {"dram_only", "cxl_only", "no_place"}
TRACE_FILES = ("decision_trace.csv", "operator_segment_trace.csv")
TRACE_REQUIRED = {
    "decision_trace.csv": {
        "run_id", "config", "rep", "unix_ts_ns", "cycle_id", "phase",
        "table_name", "chunk_id", "column_id", "segment_bytes",
        "source_tier", "target_tier", "action", "selected", "rank",
        "fits_remaining", "policy", "data_type", "bytes_per_value",
        "total_score", "access_rate_raw", "max_rate", "norm_freq",
        "access_type", "atype_score", "data_volume_bytes", "max_volume_bytes",
        "norm_volume", "is_immutable", "immutable_score", "weight_atype",
        "weight_freq", "weight_volume", "weight_immutable", "term_atype",
        "term_freq", "term_volume", "term_immutable", "estimated_cardinality",
        "operator_mask", "remaining_budget_before", "remaining_budget_after",
    },
    "operator_segment_trace.csv": {
        "run_id", "config", "rep", "query_name", "execution_id",
        "operator_id", "operator_type", "table_name", "chunk_id",
        "column_id", "estimated_rows", "rows_in", "rows_out",
        "operator_wall_ns", "actual_reads", "tier_at_execution", "attribution_level",
    },
}


def add_error(report: dict, message: str) -> None:
    report["errors"].append(message)


def add_warning(report: dict, message: str) -> None:
    report["warnings"].append(message)


def parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"true", "1", "yes"}:
        return True
    if normalized in {"false", "0", "no"}:
        return False
    raise ValueError(f"invalid boolean: {value!r}")


def parse_number(row: dict, field: str, *, integer: bool = False) -> float | int:
    value = row.get(field, "")
    if value == "":
        raise ValueError(f"empty numeric field {field}")
    return int(value) if integer else float(value)


def read_csv(path: Path) -> tuple[list[str], list[dict[str, str]]]:
    with path.open(newline="", encoding="utf-8") as handle:
        reader = csv.DictReader(handle)
        return list(reader.fieldnames or []), list(reader)


def validate_identity(config: dict, report: dict) -> None:
    config_name = str(config.get("config", config.get("configuration", "")))
    reference_only = config_name in REFERENCE_CONFIGS
    aliases = {
        "git revision": ("git_hash", "git_commit", "git_revision"),
        "binary SHA-256": ("binary_sha256", "hyrise_binary_sha256"),
        "command": ("command", "full_command"),
        "configuration": ("config", "configuration"),
        "repetition": ("rep", "repetition", "repetition_id"),
        "query schedule SHA-256": ("query_schedule_sha256", "schedule_sha256", "query_order_sha256"),
        "query schedule SHA-256": ("query_schedule_sha256", "schedule_sha256", "query_order_sha256"),
    }
    if not reference_only:
        aliases["plugin SHA-256"] = ("plugin_sha256", "plugin_binary_sha256")
        aliases["exact logical DRAM budget bytes"] = ("dram_budget_bytes", "dram_capacity_bytes")
    for label, keys in aliases.items():
        if not any(config.get(key) not in (None, "") for key in keys):
            add_error(report, f"run_config.json lacks {label} ({', '.join(keys)})")

    for key in ("binary_sha256", "hyrise_binary_sha256", "plugin_sha256", "plugin_binary_sha256", "query_schedule_sha256", "schedule_sha256", "query_order_sha256"):
        value = config.get(key)
        if value and (len(str(value)) != 64 or any(c not in "0123456789abcdefABCDEF" for c in str(value))):
            add_error(report, f"{key} is not a 64-character SHA-256 digest")


def validate_summary(run_dir: Path, report: dict) -> None:
    try:
        header, rows = read_csv(run_dir / "summary.csv")
    except Exception as exc:
        add_error(report, f"cannot parse summary.csv: {exc}")
        return
    if not rows:
        add_error(report, "summary.csv has no data row")
        return
    candidate_metrics = {"geomean_avg_ms", "geomean_ms", "throughput_qps"}
    if not candidate_metrics.intersection(header):
        add_error(report, "summary.csv lacks geomean_avg_ms/geomean_ms/throughput_qps")
    for metric in candidate_metrics.intersection(header):
        try:
            value = float(rows[0][metric])
            if not math.isfinite(value) or value <= 0:
                add_error(report, f"summary metric {metric} must be finite and positive")
        except Exception as exc:
            add_error(report, f"invalid summary metric {metric}: {exc}")


def validate_daemon_metrics(run_dir: Path, report: dict) -> None:
    required = {
        "unix_ts", "cycle_id", "hit_rate_pct", "promotes", "demotes",
        "bytes_promoted", "bytes_demoted", "daemon_cpu_ms", "replica_bytes",
    }
    try:
        header, rows = read_csv(run_dir / "daemon_metrics.csv")
    except Exception as exc:
        add_error(report, f"cannot parse daemon_metrics.csv: {exc}")
        return
    missing = sorted(required - set(header))
    if missing:
        add_error(report, f"daemon_metrics.csv missing fields: {', '.join(missing)}")
        return
    if not rows:
        add_error(report, "daemon_metrics.csv has no data rows")
        return
    previous_cycle = -1
    previous_ts = -math.inf
    for index, row in enumerate(rows, start=2):
        try:
            cycle = parse_number(row, "cycle_id", integer=True)
            timestamp = parse_number(row, "unix_ts")
            hit_rate = parse_number(row, "hit_rate_pct")
            for field in ("promotes", "demotes", "bytes_promoted", "bytes_demoted", "daemon_cpu_ms", "replica_bytes"):
                if parse_number(row, field) < 0:
                    raise ValueError(f"{field} is negative")
            if not 0 <= hit_rate <= 100:
                raise ValueError("hit_rate_pct outside [0,100]")
            if cycle < previous_cycle or timestamp < previous_ts:
                raise ValueError("cycle_id or unix_ts decreases")
            previous_cycle, previous_ts = cycle, timestamp
        except Exception as exc:
            add_error(report, f"daemon_metrics.csv row {index}: {exc}")
            if len(report["errors"]) >= 25:
                break


def validate_trace(run_dir: Path, report: dict, budget_bytes: int | None) -> None:
    for filename in TRACE_FILES:
        path = run_dir / filename
        if not path.exists():
            add_error(report, f"required trace file missing: {filename}")
            continue
        try:
            handle = path.open(newline="", encoding="utf-8")
        except Exception as exc:
            add_error(report, f"cannot open {filename}: {exc}")
            continue
        with handle:
            reader = csv.DictReader(handle)
            missing = sorted(TRACE_REQUIRED[filename] - set(reader.fieldnames or []))
            if missing:
                add_error(report, f"{filename} missing fields: {', '.join(missing)}")
                continue
            row_count = 0
            selected_bytes_by_cycle: dict[tuple[str, str], int] = {}
            for index, row in enumerate(reader, start=2):
                row_count += 1
                try:
                    if filename == "operator_segment_trace.csv":
                        level = row["attribution_level"]
                        if level not in {"measured_per_execution", "counter_delta_concurrent", "plan_reference_only"}:
                            raise ValueError(f"unknown attribution_level {level!r}")
                        actual_reads = row.get("actual_reads", "")
                        if level == "measured_per_execution" and actual_reads == "":
                            raise ValueError("measured_per_execution requires actual_reads")
                        if actual_reads != "" and int(actual_reads) < 0:
                            raise ValueError("actual_reads is negative")
                        if level == "plan_reference_only" and any(row.get(field, "") not in ("", None) for field in ("dram_accesses", "cxl_accesses", "dram_bytes", "cxl_bytes")):
                            raise ValueError("plan_reference_only must not carry measured traffic fields")
                        continue

                    config = row["config"]
                    if config not in {"our_full", "our_freq", "cost_c3_greedy"}:
                        raise ValueError(f"unknown config {config!r}")
                    selected = parse_bool(row["selected"])
                    fits_remaining = parse_bool(row["fits_remaining"])
                    parse_bool(row["is_immutable"])
                    segment_bytes = parse_number(row, "segment_bytes", integer=True)
                    before = parse_number(row, "remaining_budget_before", integer=True)
                    after = parse_number(row, "remaining_budget_after", integer=True)
                    for value, label in ((segment_bytes, "segment_bytes"), (before, "remaining_budget_before"), (after, "remaining_budget_after")):
                        if value < 0:
                            raise ValueError(f"{label} is negative")
                    if budget_bytes is not None and max(before, after) > budget_bytes:
                        raise ValueError("remaining budget exceeds declared byte budget")
                    total = parse_number(row, "total_score")
                    if not math.isfinite(float(total)):
                        raise ValueError("total_score is not finite")
                    expected_policy = {"our_full": "our_full", "our_freq": "our_no_plan", "cost_c3_greedy": "cost_c3_greedy"}[config]
                    if row["policy"] != expected_policy:
                        raise ValueError(f"policy {row['policy']!r} does not match config {config!r}")
                    for field in ("norm_freq", "norm_volume"):
                        value = float(row[field])
                        if not 0.0 <= value <= 1.0:
                            raise ValueError(f"{field} outside [0,1]")
                    if selected and row["target_tier"] == "DRAM":
                        if not fits_remaining or segment_bytes > before:
                            raise ValueError("selected DRAM segment does not strictly fit remaining budget")
                        if after != before - segment_bytes:
                            raise ValueError("remaining budget does not decrease by selected segment bytes")
                        cycle_key = (row["run_id"], row["cycle_id"])
                        selected_bytes_by_cycle[cycle_key] = selected_bytes_by_cycle.get(cycle_key, 0) + int(segment_bytes)
                    if row["action"] == "skip_no_fit" and (selected or fits_remaining):
                        raise ValueError("skip_no_fit row must be unselected with fits_remaining=false")
                    if config == "cost_c3_greedy":
                        for field in ("sequential_count", "monotonic_count", "random_count", "point_count", "dictionary_count", "cost_dram", "cost_cxl"):
                            if row.get(field, "") == "":
                                raise ValueError(f"cost_c3_greedy lacks {field}")
                        if not row.get("dictionary_handling", "").strip():
                            raise ValueError("cost_c3_greedy lacks explicit dictionary_handling")
                        expected = float(row["cost_cxl"]) - float(row["cost_dram"])
                        if not math.isclose(float(total), expected, rel_tol=1e-6, abs_tol=1e-9):
                            raise ValueError("total_score != cost_cxl - cost_dram")
                    else:
                        components = (
                            ("weight_atype", "atype_score", "term_atype"),
                            ("weight_freq", "norm_freq", "term_freq"),
                            ("weight_volume", "norm_volume", "term_volume"),
                            ("weight_immutable", "immutable_score", "term_immutable"),
                        )
                        terms = []
                        for weight, feature, term in components:
                            expected_term = float(row[weight]) * float(row[feature])
                            actual_term = float(row[term])
                            if not math.isclose(actual_term, expected_term, rel_tol=1e-9, abs_tol=1e-12):
                                raise ValueError(f"{term} != {weight} * {feature}")
                            terms.append(actual_term)
                        if not math.isclose(float(total), sum(terms), rel_tol=1e-9, abs_tol=1e-12):
                            raise ValueError("total_score != sum of semantic policy terms")
                except Exception as exc:
                    add_error(report, f"{filename} row {index}: {exc}")
                    if len(report["errors"]) >= 25:
                        break
            if row_count == 0:
                add_error(report, f"{filename} has no data rows")
            if filename == "decision_trace.csv" and budget_bytes is not None:
                for cycle_key, selected_bytes in selected_bytes_by_cycle.items():
                    if selected_bytes > budget_bytes:
                        add_error(report, f"decision_trace cycle {cycle_key} selects {selected_bytes} bytes over budget {budget_bytes}")


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path)
    parser.add_argument("--require-trace", action="store_true")
    parser.add_argument("--budget-bytes", type=int)
    parser.add_argument("--create-stamp", action="store_true", help="write validated.ok only if all checks pass")
    parser.add_argument("--report", type=Path, help="JSON report path; default is RUN_DIR/validation_report.json")
    args = parser.parse_args()

    run_dir = args.run_dir.resolve()
    report = {"run_dir": str(run_dir), "valid": False, "errors": [], "warnings": [], "files": {}}
    if not run_dir.is_dir():
        add_error(report, "run directory does not exist")
    else:
        for filename in BASE_FILES:
            path = run_dir / filename
            if not path.is_file() or path.stat().st_size == 0:
                add_error(report, f"required non-empty file missing: {filename}")
            else:
                report["files"][filename] = {"bytes": path.stat().st_size, "sha256": file_hash(path)}

        config = {}
        if (run_dir / "run_config.json").is_file():
            try:
                config = json.loads((run_dir / "run_config.json").read_text(encoding="utf-8"))
                validate_identity(config, report)
            except Exception as exc:
                add_error(report, f"cannot parse run_config.json: {exc}")
        config_name = str(config.get("config", config.get("configuration", "")))
        if config_name not in REFERENCE_CONFIGS:
            for filename in MIGRATION_FILES:
                path = run_dir / filename
                if not path.is_file() or path.stat().st_size == 0:
                    add_error(report, f"required non-empty migration file missing: {filename}")
                else:
                    report["files"][filename] = {"bytes": path.stat().st_size, "sha256": file_hash(path)}
        else:
            for filename in MIGRATION_FILES:
                if not (run_dir / filename).is_file():
                    add_warning(report, f"reference configuration omits non-applicable {filename}")
        if (run_dir / "summary.csv").is_file():
            validate_summary(run_dir, report)
        if (run_dir / "exit_code.txt").is_file():
            try:
                if int((run_dir / "exit_code.txt").read_text(encoding="utf-8").strip()) != 0:
                    add_error(report, "exit_code.txt is non-zero")
            except Exception as exc:
                add_error(report, f"invalid exit_code.txt: {exc}")
        if (run_dir / "daemon_metrics.csv").is_file():
            validate_daemon_metrics(run_dir, report)

        config_budget = config.get("dram_budget_bytes", config.get("dram_capacity_bytes"))
        if config_budget not in (None, ""):
            try:
                config_budget = int(config_budget)
                if config_budget <= 0:
                    raise ValueError("must be positive")
            except Exception as exc:
                add_error(report, f"invalid exact logical DRAM budget bytes: {exc}")
                config_budget = None
        if args.budget_bytes is not None and config_budget is not None and args.budget_bytes != config_budget:
            add_error(report, "--budget-bytes disagrees with run_config exact logical DRAM budget")
        effective_budget = args.budget_bytes if args.budget_bytes is not None else config_budget
        trace_expected = args.require_trace or str(config.get("trace", "")).lower() in {"on", "true", "1", "yes"}
        if trace_expected:
            validate_trace(run_dir, report, effective_budget)
            for filename in TRACE_FILES:
                path = run_dir / filename
                if path.is_file():
                    report["files"][filename] = {"bytes": path.stat().st_size, "sha256": file_hash(path)}

    report["valid"] = not report["errors"]
    report_path = (args.report or run_dir / "validation_report.json").resolve()
    report_path.parent.mkdir(parents=True, exist_ok=True)
    report_path.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    if report["valid"] and args.create_stamp:
        validator_path = Path(__file__).resolve()
        stamp = {
            "validator": str(validator_path),
            "validator_sha256": file_hash(validator_path),
            "validation_report": str(report_path),
            "validation_report_sha256": file_hash(report_path),
        }
        (run_dir / "validated.ok").write_text(json.dumps(stamp, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2, sort_keys=True))
    return 0 if report["valid"] else 2


if __name__ == "__main__":
    sys.exit(main())
