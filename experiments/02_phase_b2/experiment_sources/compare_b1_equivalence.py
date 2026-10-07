#!/usr/bin/env python3
"""EDBT v4 Phase B — commit B1 trace-off equivalence comparison.

Compares the B1-instrumented binary (trace OFF) against the clean base build
(0f182ad07, fresh worktree build).

Method notes (derived from the first MISMATCH iteration, kept for the record):
- benchmark_semantic_placement.csv carries the FINAL accumulated access
  counters, which count the whole Shuffled run; counter jitter (~0.005%) is
  pre-existing run-to-run nondeterminism and appears INSIDE the base-vs-base
  control pair as well. Byte-comparing the file is therefore the wrong
  invariant. The decision invariant is the segment -> decision MAPPING
  (table,chunk,col) -> (access_type,target_tier,create_replica,is_immutable,
  data_volume), which is order-insensitive.
- Even the mapping-adjacent row ORDER can swap by one position when two
  segments have near-equal profiling frequencies (observed identically in the
  base-vs-base control). This is reported but not treated as a mismatch.
- run.log lines embed run-specific paths (output dir, plugin path). Paths are
  normalized before comparison; timing/counter lines are filtered.

Pairs:
  A) daemon OFF pair (deterministic decision path): mapping must be identical,
     normalized filtered logs identical.
  B) base-vs-base control (daemon ON): demonstrates pre-existing
     nondeterminism ([Daemon] cycle lines differ run-to-run; frequency column
     jitters) on the UNPATCHED binary.
  C) base vs B1 with daemon ON: mapping must be identical.
"""
from __future__ import annotations

import hashlib
import json
import re
import sys
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
EQ = ROOT / "results_edbt_v4/01_phase_b/smoke_runs/commit_b1_equivalence"

# Lines removed before run.log comparison: per-query results, daemon cycle
# accounting (counter-driven, nondeterministic), table-load timings.
TIMING_OR_NOISE = re.compile(
    r"Results for|-> *Executed|additional runs failed|\[Daemon\] Cycle|"
    r"Loaded table|Loading/Generating|Generating test |took|µs|ms\)|"
    r"\[Daemon\] Thread exiting|Benchmarking [0-9]"
)

# Run-specific path normalization: output dir per run, binary/plugin location
# (main-tree build vs base worktree build), and JSON output paths.
PATH_NORM = [
    (re.compile(r"/commit_b1_equivalence/\w+"), "/commit_b1_equivalence/RUN"),
    (re.compile(r"/home/dameng/CXL_migration/hyrise_base_wt/build_base"), "BINDIR"),
    (re.compile(r"/home/dameng/CXL_migration/hyrise/build"), "BINDIR"),
]


def normalized(line: str) -> str:
    for rx, repl in PATH_NORM:
        line = rx.sub(repl, line)
    return line.rstrip()


def filtered_log(path: Path) -> list[str]:
    out = []
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if TIMING_OR_NOISE.search(line):
            continue
        out.append(normalized(line))
    return out


def daemon_lines(path: Path) -> list[str]:
    return [l.strip() for l in (path / "run.log").read_text(encoding="utf-8", errors="replace").splitlines()
            if "[Daemon] Cycle" in l]


def sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def placement_facts(path: Path) -> dict[tuple[str, int, int], tuple[int, int, int, int, int]]:
    """segment -> (access_type, target_tier, create_replica, is_immutable, data_volume)."""
    facts = {}
    with path.open(encoding="utf-8") as f:
        header = f.readline().strip().split(",")
        idx = {name: i for i, name in enumerate(header)}
        for line in f:
            if not line.strip():
                continue
            cols = line.rstrip("\n").split(",")
            key = (cols[idx["table_name"]], int(cols[idx["chunk_id"]]), int(cols[idx["column_id"]]))
            facts[key] = (int(cols[idx["access_type"]]), int(cols[idx["target_tier"]]),
                          int(cols[idx["create_replica"]]), int(cols[idx["is_immutable"]]),
                          int(cols[idx["data_volume"]]))
    return facts


def frequency_column(path: Path) -> list[int]:
    with path.open(encoding="utf-8") as f:
        header = f.readline().strip().split(",")
        i = header.index("access_frequency")
        return [int(l.rstrip("\n").split(",")[i]) for l in f if l.strip()]


def pair(label: str, a: str, b: str) -> dict:
    pa = EQ / a / "benchmark_semantic_placement.csv"
    pb = EQ / b / "benchmark_semantic_placement.csv"
    fa, fb = placement_facts(pa), placement_facts(pb)
    freq_a, freq_b = frequency_column(pa), frequency_column(pb)
    only_a = {k: v for k, v in fa.items() if fb.get(k) != v}
    only_b = {k: v for k, v in fb.items() if fa.get(k) != v}
    freq_jitter = sum(1 for x, y in zip(freq_a, freq_b) if x != y)
    la, lb = filtered_log(EQ / a / "run.log"), filtered_log(EQ / b / "run.log")
    # Multiset comparison: parallel table loading emits "will be encoded as ..."
    # notice lines in nondeterministic ORDER (stdout race, also present in the
    # base-vs-base control pair). Line multiset equality is the invariant;
    # ordered equality is reported informationally.
    entry = {
        "a": a,
        "b": b,
        "placement_csv_a_sha256": sha256(pa),
        "placement_csv_b_sha256": sha256(pb),
        "segment_count_a": len(fa),
        "segment_count_b": len(fb),
        "decision_mapping_identical": not only_a and not only_b and len(fa) == len(fb),
        "decision_mapping_diff_a": sorted(map(str, only_a.keys()))[:6],
        "decision_mapping_diff_b": sorted(map(str, only_b.keys()))[:6],
        "frequency_column_differing_rows": freq_jitter,
        "frequency_total_a": sum(freq_a),
        "frequency_total_b": sum(freq_b),
        "filtered_log_identical": sorted(la) == sorted(lb),
        "filtered_log_ordered_identical": la == lb,
        "filtered_log_line_counts": [len(la), len(lb)],
    }
    if sorted(la) != sorted(lb):
        entry["filtered_log_diff"] = [f"-{x}" for x in la if x not in lb][:8] + \
                                     [f"+{x}" for x in lb if x not in la][:8]
    return entry


def main() -> int:
    report = {"pairs": {}, "verdict": None}

    report["pairs"]["A_daemon0_base_vs_b1"] = pair("A_daemon0_base_vs_b1", "eq_base_daemon0", "eq_b1_daemon0")
    report["pairs"]["B_control_base_vs_base_d1"] = pair("B_control_base_vs_base_d1", "eq_base_d1_run1", "eq_base_d1_run2")
    report["pairs"]["C_daemon1_base_vs_b1"] = pair("C_daemon1_base_vs_b1", "eq_base_d1_run1", "eq_b1_d1")

    # Control pair must DEMONSTRATE the pre-existing nondeterminism class.
    da, db = daemon_lines(EQ / "eq_base_d1_run1"), daemon_lines(EQ / "eq_base_d1_run2")
    report["control_daemon_line_variability"] = {
        "base_run1_daemon_lines": len(da),
        "base_run2_daemon_lines": len(db),
        "daemon_lines_differ_between_base_runs": da != db,
    }

    p = report["pairs"]
    ok = (
        p["A_daemon0_base_vs_b1"]["decision_mapping_identical"]
        and p["A_daemon0_base_vs_b1"]["filtered_log_identical"]
        and p["B_control_base_vs_base_d1"]["decision_mapping_identical"]
        and p["C_daemon1_base_vs_b1"]["decision_mapping_identical"]
        and report["control_daemon_line_variability"]["daemon_lines_differ_between_base_runs"]
    )
    report["verdict"] = "EQUIVALENT_ON_DETERMINISTIC_PATH" if ok else "MISMATCH"

    out = EQ / "EQ_REPORT.json"
    out.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if ok else 2


if __name__ == "__main__":
    sys.exit(main())
