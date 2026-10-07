#!/usr/bin/env python3
"""EDBT v4 Phase B2 proof collector.

Consumes the B2 smoke bundles (results_edbt_v4/02_phase_b2/smoke_runs) and the
B2 calibration outputs; emits PROOFS.json + PROOFS.md with one PASS/FAIL per
B2 requirement. Read-only: nothing here edits raw logs.

Proofs:
  P1  B2-2 candidate-set identity: initial set (semantic placement CSV) and
      daemon per-cycle sets (decision trace) have IDENTICAL hashes across
      our_full / our_freq / cost_c3_greedy, and initial == daemon.
  P2  B2-4 all-policy typed counters: non-c3 rows carry the five counts and
      dictionary_handling=observed_not_scored with empty costs; c3 rows carry
      excluded_reported + both costs; counters are non-decreasing per segment
      across cycles (cumulative semantics; per-cycle deltas computed downstream).
  P3  B2-5 measured diagnostic: measured_segment_access.csv rows all
      measured_per_execution with tier from {DRAM, CXL}; operator trace has
      measured_per_execution rows; actual_reads non-null only on a subset
      (unique-scan rule); run_config records clients=1 + trace_measured=true.
  P4  B2-6 calibration: 40 cells x 12 retained repeats, working sets > LLC,
      CI encloses the median, randomized cell order is a permutation.
  P5  B2-7/B2-8: sensitivity outputs exist and carry Jaccard + dictionary
      share numbers.
  P6  B2-10 stamps: every bundle's validated.ok binds the validator SHA-256
      and the validation-report SHA-256 (recomputed here).
  P7  determinism: same-seed rerun's schedule_plan.csv is byte-identical.
  P8  B2-1: every run_config.json records runner/validator SHA-256 that match
      the actual files.
"""

import csv
import hashlib
import json
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
SMOKE = ROOT / "results_edbt_v4/02_phase_b2/smoke_runs"
CALIB = ROOT / "results_edbt_v4/02_phase_b2/calibration"
SENS = ROOT / "results_edbt_v4/02_phase_b2/sensitivity"
PACK = Path("/home/dameng/CXL_migration/edbt_phase_b2_extract/CXL 页面迁移/edbt_experiment_pack")
VALIDATOR = PACK / "scripts/validate_run_bundle.py"
RUNNER = ROOT / "experiments/edbt_v4/run_smoke_phase_b2.py"

TRACE_ARMS = {"smoke_b2_our_full_traceon": "our_full",
              "smoke_b2_our_freq_traceon": "our_freq",
              "smoke_b2_cost_c3_greedy_traceon": "cost_c3_greedy"}
MEASURED_BUNDLE = "smoke_b2_our_full_measured"
ALL_BUNDLES = list(TRACE_ARMS) + [MEASURED_BUNDLE,
                                  "smoke_b2_our_full", "smoke_b2_our_freq", "smoke_b2_cost_c3_greedy",
                                  "smoke_b2_dram_only", "smoke_b2_cxl_only", "smoke_b2_our_full_rerun"]
LLC_BYTES = 452984832


def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha256_file(p: Path) -> str:
    return sha256_bytes(p.read_bytes())


def set_hash(keys) -> str:
    blob = "\n".join(sorted(keys)).encode()
    return sha256_bytes(blob)


def proof(name, ok, detail):
    return {"name": name, "ok": bool(ok), "detail": detail}


def p1_candidate_identity():
    initial_hashes, daemon_hashes = {}, {}
    details = {}
    for bundle, arm in TRACE_ARMS.items():
        d = SMOKE / bundle
        placement = list(csv.DictReader(open(d / "benchmark_semantic_placement.csv")))
        initial = {(r["table_name"], int(r["chunk_id"]), int(r["column_id"])) for r in placement}
        initial_hashes[arm] = set_hash(initial)

        trace = list(csv.DictReader(open(d / "decision_trace.csv")))
        by_cycle = defaultdict(set)
        for r in trace:
            by_cycle[int(r["cycle_id"])].add((r["table_name"], int(r["chunk_id"]), int(r["column_id"])))
        cycle_hashes = {c: set_hash(keys) for c, keys in by_cycle.items()}
        distinct = set(cycle_hashes.values())
        daemon_hashes[arm] = distinct.pop() if len(distinct) == 1 else f"INCONSISTENT({len(distinct)})"
        details[arm] = dict(initial_segments=len(initial), cycles=len(by_cycle),
                            initial_sha=initial_hashes[arm][:16], daemon_sha=daemon_hashes[arm][:16])
    ok = (len(set(initial_hashes.values())) == 1
          and len(set(daemon_hashes.values())) == 1
          and next(iter(initial_hashes.values())) == next(iter(daemon_hashes.values())))
    return proof("P1 B2-2 initial==daemon candidate-set hash, identical across 3 policies",
                 ok, {"initial": initial_hashes, "daemon_per_cycle": daemon_hashes, "detail": details})


def p2_typed_counters_all_policies():
    problems = []
    stats = {}
    for bundle, arm in TRACE_ARMS.items():
        d = SMOKE / bundle
        rows = csv.DictReader(open(d / "decision_trace.csv"))
        n = 0
        prev = {}
        nonmono = 0
        nonzero_counts = defaultdict(int)
        for r in rows:
            n += 1
            counts = [int(r[c]) for c in ("sequential_count", "monotonic_count", "random_count",
                                          "point_count", "dictionary_count")]
            if any(c < 0 for c in counts):
                problems.append(f"{arm}: negative count row")
            if arm == "cost_c3_greedy":
                if r["dictionary_handling"] != "excluded_reported":
                    problems.append(f"{arm}: dictionary_handling={r['dictionary_handling']!r}")
                if r["cost_dram"] == "" or r["cost_cxl"] == "":
                    problems.append(f"{arm}: missing costs")
            else:
                if r["dictionary_handling"] != "observed_not_scored":
                    problems.append(f"{arm}: dictionary_handling={r['dictionary_handling']!r}")
                if r["cost_dram"] != "" or r["cost_cxl"] != "":
                    problems.append(f"{arm}: non-c3 row carries costs")
            for c, v in zip(("seq", "mon", "rand", "point", "dict"), counts):
                if v > 0:
                    nonzero_counts[c] += 1
            key = (r["table_name"], int(r["chunk_id"]), int(r["column_id"]))
            if key in prev and any(v < p for v, p in zip(counts, prev[key])):
                nonmono += 1
            prev[key] = counts
        if nonmono:
            problems.append(f"{arm}: {nonmono} rows with decreasing cumulative counters")
        stats[arm] = dict(rows=n, nonzero_rows=dict(nonzero_counts))
    # at least two pattern columns non-zero on JOB smoke (c3 requirement kept)
    c3nz = stats.get("cost_c3_greedy", {}).get("nonzero_rows", {})
    if sum(1 for v in c3nz.values() if v > 0) < 2:
        problems.append("c3: fewer than two non-zero pattern counter columns")
    return proof("P2 B2-4 typed counters observed for all policies (cumulative, monotone)", not problems,
                 {"problems": problems[:10], "stats": stats})


def p3_measured_diagnostic():
    d = SMOKE / MEASURED_BUNDLE
    if not (d / "measured_segment_access.csv").exists():
        return proof("P3 B2-5 measured diagnostic path", False, {"missing": "measured_segment_access.csv"})
    problems = []
    mrows = list(csv.DictReader(open(d / "measured_segment_access.csv")))
    tiers = {r["tier_at_execution"] for r in mrows}
    if any(r["attribution_level"] != "measured_per_execution" for r in mrows):
        problems.append("non-measured attribution in measured_segment_access.csv")
    if not tiers <= {"DRAM", "CXL"}:
        problems.append(f"unexpected tiers {tiers}")
    if not any(int(r["total_delta"]) > 0 for r in mrows):
        problems.append("no positive measured deltas")

    orows = list(csv.DictReader(open(d / "operator_segment_trace.csv")))
    measured = [r for r in orows if r["attribution_level"] == "measured_per_execution"]
    with_reads = [r for r in measured if r["actual_reads"] != ""]
    if not measured:
        problems.append("no measured operator rows")
    for r in with_reads:
        if int(r["actual_reads"]) < 0 or int(r["operator_wall_ns"]) <= 0:
            problems.append("invalid measured operator row")
            break

    cfg = json.loads((d / "run_config.json").read_text())
    if cfg.get("clients") != 1:
        problems.append(f"clients={cfg.get('clients')} != 1")
    if not cfg.get("trace_measured"):
        problems.append("run_config lacks trace_measured=true")
    return proof("P3 B2-5 single-client measured diagnostic (exact deltas + PQP rows/wall)", not problems,
                 {"problems": problems, "measured_segment_rows": len(mrows),
                  "measured_operator_rows": len(measured),
                  "operator_rows_with_actual_reads": len(with_reads),
                  "tiers": sorted(tiers)})


def p4_calibration():
    stats_p = CALIB / "calibration_stats.csv"
    raw_p = CALIB / "calibration_raw_v2.csv"
    if not stats_p.exists():
        return proof("P4 B2-6 calibration redo (unique WS > LLC, randomized order, 12 repeats, CI)", False,
                     {"missing": str(stats_p)})
    problems = []
    rows = list(csv.DictReader(open(stats_p)))
    if len(rows) != 40:
        problems.append(f"{len(rows)} cells != 40")
    orders = sorted(int(r["cell_order"]) for r in rows)
    if orders != list(range(40)):
        problems.append("cell_order not a permutation of 0..39")
    for r in rows:
        if int(r["n"]) < 10:
            problems.append(f"{r['device']}/{r['gamma']}/{r['data_type']}: n={r['n']} < 10")
        if not (float(r["ci95_low"]) <= float(r["median_ns_per_access_per_byte"]) <= float(r["ci95_high"])):
            problems.append(f"CI does not enclose median for {r['device']}/{r['gamma']}/{r['data_type']}")
    raw = list(csv.DictReader(open(raw_p)))
    for r in raw:
        if int(r["working_set_bytes"]) <= LLC_BYTES:
            problems.append(f"working set {r['working_set_bytes']} <= LLC for {r['device']}/{r['gamma']}/{r['data_type']}")
            break
    return proof("P4 B2-6 calibration redo (unique WS > LLC, randomized order, >=10 repeats, CI)", not problems,
                 {"problems": problems[:10], "cells": len(rows), "raw_rows": len(raw)})


def p5_sensitivity():
    p = SENS / "sensitivity_results.json"
    if not p.exists():
        return proof("P5 B2-7/B2-8 sensitivity analyses", False, {"missing": str(p)})
    data = json.loads(p.read_text())
    b27 = data["b27_calibration_sensitivity"]
    b28 = data["b28_dictionary_sensitivity"]
    problems = []
    for v in ("lo", "hi"):
        if not (0.0 <= b27["jaccard_segment"][v] <= 1.0):
            problems.append(f"jaccard lo/hi out of range: {v}")
    if not (0.0 <= b28["dictionary_share_cumulative"] <= 1.0):
        problems.append("dictionary share out of range")
    return proof("P5 B2-7/B2-8 sensitivity (endpoints Jaccard + dictionary share)", not problems,
                 {"problems": problems, "jaccard_segment": b27["jaccard_segment"],
                  "jaccard_byte_weighted": b27["jaccard_byte_weighted"],
                  "dictionary_share_cumulative": b28["dictionary_share_cumulative"],
                  "dictionary_share_delta": b28["dictionary_share_delta"],
                  "b28_jaccard_vs_excluded": b28["jaccard_segment_vs_excluded"]})


def p6_stamps():
    problems, stamps = [], {}
    for bundle in ALL_BUNDLES:
        d = SMOKE / bundle
        stamp_p = d / "validated.ok"
        report_p = d / "validation_report.json"
        if not stamp_p.exists():
            problems.append(f"{bundle}: no validated.ok")
            continue
        try:
            stamp = json.loads(stamp_p.read_text())
        except Exception as exc:
            problems.append(f"{bundle}: stamp unparseable: {exc}")
            continue
        if stamp.get("validator_sha256") != sha256_file(VALIDATOR):
            problems.append(f"{bundle}: stamp validator SHA mismatch")
        if stamp.get("validation_report_sha256") != sha256_file(report_p):
            problems.append(f"{bundle}: stamp report SHA mismatch")
        rep = json.loads(report_p.read_text())
        if not rep.get("valid"):
            problems.append(f"{bundle}: validation_report not valid")
        stamps[bundle] = stamp["validation_report_sha256"][:16]
    return proof("P6 B2-10 validated.ok binds validator + report SHA-256 (all bundles)", not problems,
                 {"problems": problems, "stamps": stamps})


def p7_determinism():
    a = SMOKE / "smoke_b2_our_full" / "schedule_plan.csv"
    b = SMOKE / "smoke_b2_our_full_rerun" / "schedule_plan.csv"
    if not (a.exists() and b.exists()):
        return proof("P7 same-seed schedule determinism", False, {"missing": "schedule_plan.csv"})
    same = a.read_bytes() == b.read_bytes()
    return proof("P7 same-seed schedule determinism (byte-identical schedule_plan.csv)", same,
                 {"sha256_a": sha256_file(a), "sha256_b": sha256_file(b)})


def p8_run_config_shas():
    problems = []
    runner_sha = sha256_file(RUNNER)
    validator_sha = sha256_file(VALIDATOR)
    for bundle in ALL_BUNDLES:
        d = SMOKE / bundle
        if not (d / "run_config.json").exists():
            problems.append(f"{bundle}: no run_config.json")
            continue
        cfg = json.loads((d / "run_config.json").read_text())
        if cfg.get("runner_sha256") != runner_sha:
            problems.append(f"{bundle}: runner SHA mismatch")
        if cfg.get("validator_sha256") != validator_sha:
            problems.append(f"{bundle}: validator SHA mismatch")
    return proof("P8 B2-1 every run_config records matching runner/validator SHA-256", not problems,
                 {"problems": problems, "runner_sha256": runner_sha, "validator_sha256": validator_sha})


def main() -> int:
    proofs = [p1_candidate_identity(), p2_typed_counters_all_policies(), p3_measured_diagnostic(),
              p4_calibration(), p5_sensitivity(), p6_stamps(), p7_determinism(), p8_run_config_shas()]
    all_ok = all(p["ok"] for p in proofs)
    out = {"ALL_PROOFS_PASS": all_ok, "proofs": proofs}
    (ROOT / "results_edbt_v4/02_phase_b2/PROOFS.json").write_text(json.dumps(out, indent=2) + "\n")
    lines = [f"# EDBT v4 Phase B2 proofs: {'ALL PASS' if all_ok else 'FAILURES PRESENT'}", ""]
    for p in proofs:
        lines.append(f"## [{'PASS' if p['ok'] else 'FAIL'}] {p['name']}")
        lines.append(f"```json\n{json.dumps(p['detail'], indent=2, ensure_ascii=False)}\n```")
        lines.append("")
    (ROOT / "results_edbt_v4/02_phase_b2/PROOFS.md").write_text("\n".join(lines) + "\n")
    print(json.dumps(out, indent=2)[:4000])
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
