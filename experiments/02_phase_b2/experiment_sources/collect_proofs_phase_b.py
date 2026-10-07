#!/usr/bin/env python3
"""EDBT v4 Phase B: collect the ten required proofs from the smoke bundles.

Reads the bundles produced by run_smoke_phase_b.py (plus the calibration CSV)
and writes results_edbt_v4/01_phase_b/PROOFS.json and PROOFS.md.

Proofs (numbering follows PROMPT_FOR_PHASE_B_AGENT.md):
 1  the three migration configs rank the SAME candidate segment set
 2  deterministic tie-break + strict fit never exceed the byte budget per cycle
 3  semantic scores recomputable from atype/freq/volume/immutable (1e-9)
 4  C3 scores recomputable from the four pattern counters + calibration
 5  dictionary counters reported AND excluded (excluded_reported)
 6  >= 2 pattern counters non-zero on traced segments
 7  same schedule seed -> identical plan + manifest prefix; different seed -> fork
 8  dram_only / cxl_only residency within the pre-declared 5% off-target bound
 9  operator rows declare attribution_level; unavailable reads stay null (empty)
10  CXL device / platform identity evidence exists in environment/
"""
from __future__ import annotations

import csv
import hashlib
import io
import json
import math
import sys
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
PHASE_B = ROOT / "results_edbt_v4/01_phase_b"
RUNS = PHASE_B / "smoke_runs"
CALIB = PHASE_B / "calibration/calibration.csv"
BUDGET = 3725884160
# Trace `data_type` prints the DataType enum name in lowercase (magic_enum);
# plugin maps the enum to calibration grid keys (semantic_placement_plugin.cpp B3).
DTYPE_TO_CALIB = {"int": "int32", "long": "int64", "float": "float32",
                  "double": "float64", "string": "string16", "null": "int64"}
CALIB_BYTES = {"int32": 4, "int64": 8, "float32": 4, "float64": 8, "string16": 16}
GAMMAS = ("sequential", "monotonic", "random", "point")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_csv_rows(path: Path):
    with path.open(newline="", encoding="utf-8") as f:
        yield from csv.DictReader(f)


def read_plan(path: Path) -> list[dict]:
    """schedule_plan.csv: meta header line + meta row, then the real header + rows."""
    lines = path.read_text().splitlines()
    assert lines[0] == "seed,item_count,plan_length", lines[0]
    reader = csv.DictReader(io.StringIO("\n".join(lines[2:])))
    assert reader.fieldnames == ["submission_index", "item_id", "item_name"], reader.fieldnames
    return list(reader)


def load_calibration() -> dict[tuple[str, str, str], float]:
    rates = {}
    for row in read_csv_rows(CALIB):
        rates[(row["device"], row["gamma"], row["data_type"])] = float(row["ns_per_access_per_byte"])
    return rates


def close(actual: float, expected: float, rel: float, abs_tol: float) -> bool:
    return math.isclose(actual, expected, rel_tol=rel, abs_tol=abs_tol)


def candidate_key(row) -> tuple:
    return (row["table_name"], int(row["chunk_id"]), int(row["column_id"]))


def set_digest(keys) -> str:
    keys = sorted(keys)
    return hashlib.sha256("|".join(f"{t}:{c}:{k}" for t, c, k in keys).encode()).hexdigest()


def first_cycle_candidates(path: Path) -> tuple[str, list[int]]:
    """Candidate set per daemon cycle of one decision trace; returns
    (hash of first cycle's set, per-cycle sizes)."""
    per_cycle: dict[str, set] = defaultdict(set)
    for row in read_csv_rows(path):
        per_cycle[row["cycle_id"]].add(candidate_key(row))
    sizes = [len(per_cycle[c]) for c in sorted(per_cycle, key=int)]
    return set_digest(per_cycle[sorted(per_cycle, key=int)[0]]), sizes


def proof_1(proofs: dict) -> None:
    """All three migration configs rank the SAME candidate set.

    Two layers, both reported:
    * mapper layer (what the policies actually compete on): the per-cycle
      candidate set of the decision trace — must be identical across all three
      configs;
    * initial placement pass: our_full/cost_c3_greedy derive candidates from
      plan analysis (plan-referenced segments), our_freq (--ablate_d1, no plan)
      enumerates the whole storage — a strict SUPERSET by design (v3 semantics).
      The relation is verified, the difference documented, not hidden.
    """
    trace_bundles = {"our_full": "smoke_our_full_traceon",
                     "our_freq": "smoke_our_freq_traceon",
                     "cost_c3_greedy": "smoke_cost_c3_greedy_traceon"}
    mapper_hashes, mapper_sizes = {}, {}
    for config, bundle in trace_bundles.items():
        digest, sizes = first_cycle_candidates(RUNS / bundle / "decision_trace.csv")
        mapper_hashes[config] = digest
        mapper_sizes[config] = {"cycles": len(sizes), "min": min(sizes), "max": max(sizes)}
    initial_sets = {}
    for config, bundle in (("our_full", "smoke_our_full"),
                           ("our_freq", "smoke_our_freq"),
                           ("cost_c3_greedy", "smoke_cost_c3_greedy")):
        keys = {candidate_key(row)
                for row in read_csv_rows(RUNS / bundle / "benchmark_semantic_placement.csv")}
        initial_sets[config] = {"sha256": set_digest(keys), "segments": len(keys)}
    full_keys = None
    if initial_sets["our_freq"]["segments"] >= initial_sets["our_full"]["segments"]:
        full_keys = {candidate_key(row)
                     for row in read_csv_rows(RUNS / "smoke_our_freq" / "benchmark_semantic_placement.csv")}
    plan_keys = {candidate_key(row)
                 for row in read_csv_rows(RUNS / "smoke_our_full" / "benchmark_semantic_placement.csv")}
    is_superset = full_keys is not None and plan_keys.issubset(full_keys)
    proofs["1_candidate_set"] = {
        "mapper_layer": {
            "description": "per-daemon-cycle candidate set (decision trace), the set the "
                           "policies actually rank",
            "candidate_set_sha256": mapper_hashes, "per_cycle_sizes": mapper_sizes,
            "identical_across_configs": len(set(mapper_hashes.values())) == 1,
        },
        "initial_placement_layer": {
            "description": "first placement pass before the benchmark starts",
            "sets": initial_sets,
            "our_full_equals_cost_c3_greedy": (initial_sets["our_full"]["sha256"]
                                               == initial_sets["cost_c3_greedy"]["sha256"]),
            "our_freq_is_strict_superset": is_superset,
            "note": "our_freq (--ablate_d1) has no plan info, so its initial pass "
                    "enumerates the whole storage — the plan-referenced set of the other "
                    "two arms is a subset of it. From the first daemon cycle onward all "
                    "arms rank the full storage set (mapper layer).",
        },
        "pass": (len(set(mapper_hashes.values())) == 1
                 and initial_sets["our_full"]["sha256"] == initial_sets["cost_c3_greedy"]["sha256"]
                 and is_superset),
    }


def proof_2_3(proofs: dict) -> None:
    """Strict-fit budget + comparator order (P2) and semantic score recompute (P3)."""
    path = RUNS / "smoke_our_full_traceon" / "decision_trace.csv"
    per_cycle_selected = defaultdict(int)
    tie_violations = 0
    fit_violations = 0
    term_failures = 0
    worst_term_err = 0.0
    worst_total_err = 0.0
    term_rows = 0
    cycles: dict[str, list] = defaultdict(list)
    for row in read_csv_rows(path):
        cycles[row["cycle_id"]].append(row)
    for rows in cycles.values():
        rows.sort(key=lambda r: int(r["rank"]))
        prev = None
        for row in rows:
            score = float(row["total_score"])
            key3 = (row["table_name"], int(row["chunk_id"]), int(row["column_id"]))
            if prev is not None:
                p_score, p_key = prev
                if not (p_score > score or (p_score == score and p_key < key3)):
                    tie_violations += 1
            prev = (score, key3)
            seg_bytes = int(row["segment_bytes"])
            before = int(row["remaining_budget_before"])
            after = int(row["remaining_budget_after"])
            selected = row["selected"] == "true"
            fits = row["fits_remaining"] == "true"
            # strict fit: selected  <=>  segment fits the remaining budget at its rank
            if selected != fits or fits != (seg_bytes <= before):
                fit_violations += 1
            if selected and after != before - seg_bytes:
                fit_violations += 1
            if not selected and after != before:
                fit_violations += 1
            if selected and row["target_tier"] == "DRAM":
                per_cycle_selected[(row["run_id"], row["cycle_id"])] += seg_bytes
            # P3: recompute semantic score from the four published terms
            terms = [float(row["term_atype"]), float(row["term_freq"]),
                     float(row["term_volume"]), float(row["term_immutable"])]
            ok = True
            for w, feat, term in (("weight_atype", "atype_score", "term_atype"),
                                  ("weight_freq", "norm_freq", "term_freq"),
                                  ("weight_volume", "norm_volume", "term_volume"),
                                  ("weight_immutable", "immutable_score", "term_immutable")):
                expected_term = float(row[w]) * float(row[feat])
                err = abs(float(row[term]) - expected_term)
                worst_term_err = max(worst_term_err, err)
                if not close(float(row[term]), expected_term, rel=1e-9, abs_tol=1e-12):
                    ok = False
            err = abs(score - sum(terms))
            worst_total_err = max(worst_total_err, err)
            if not ok or not close(score, sum(terms), rel=1e-9, abs_tol=1e-12):
                term_failures += 1
            term_rows += 1
    over_budget = {f"{rid}:{cid}": b for (rid, cid), b in per_cycle_selected.items() if b > BUDGET}
    max_cycle_bytes = max(per_cycle_selected.values()) if per_cycle_selected else 0
    proofs["2_strict_fit_budget"] = {
        "budget_bytes": BUDGET, "cycles_checked": len(cycles),
        "tie_break_violations": tie_violations, "fit_invariant_violations": fit_violations,
        "max_selected_bytes_in_any_cycle": max_cycle_bytes,
        "cycles_over_budget": over_budget,
        "pass": tie_violations == 0 and fit_violations == 0 and not over_budget,
    }
    proofs["3_semantic_recompute"] = {
        "rows_checked": term_rows, "recompute_failures": term_failures,
        "max_abs_term_error": worst_term_err, "max_abs_total_error": worst_total_err,
        "tolerance": "math.isclose(rel_tol=1e-9, abs_tol=1e-12)",
        "pass": term_failures == 0,
    }


def proof_4_5_6(proofs: dict) -> None:
    """C3 recompute (P4), dictionary excluded_reported (P5), counters >=2 (P6)."""
    rates = load_calibration()
    path = RUNS / "smoke_cost_c3_greedy_traceon" / "decision_trace.csv"
    cost_failures = 0
    total_failures = 0
    worst_cost_rel = 0.0
    worst_total_err = 0.0
    rows_checked = 0
    dict_rows = 0
    dict_sum = 0
    handling_values = Counter()
    counter_rows_ge2 = 0
    total_rows = 0
    gamma_totals = Counter()
    for row in read_csv_rows(path):
        total_rows += 1
        handling_values[row["dictionary_handling"]] += 1
        if row["dictionary_count"]:
            dict_count = int(row["dictionary_count"])
            if dict_count > 0:
                dict_rows += 1
                dict_sum += dict_count
        counts = {g: int(row[f"{g}_count"]) for g in GAMMAS}
        nonzero = sum(1 for v in counts.values() if v > 0)
        if nonzero >= 2:
            counter_rows_ge2 += 1
        for g, v in counts.items():
            gamma_totals[g] += v
        calib_key = DTYPE_TO_CALIB[row["data_type"]]
        bpv = float(row["bytes_per_value"])
        if bpv != CALIB_BYTES[calib_key]:
            cost_failures += 1  # trace bytes_per_value disagrees with the mapped dtype width
            rows_checked += 1
            continue
        row_cost_ok = True
        for device, col in (("dram", "cost_dram"), ("cxl", "cost_cxl")):
            expected = sum(rates[(device, g, calib_key)] * counts[g] * bpv for g in GAMMAS)
            actual = float(row[col])
            denom = max(abs(expected), 1e-12)
            worst_cost_rel = max(worst_cost_rel, abs(actual - expected) / denom)
            if not close(actual, expected, rel=1e-9, abs_tol=1e-6):
                row_cost_ok = False
        if not row_cost_ok:
            cost_failures += 1
        total = float(row["total_score"])
        expected_total = float(row["cost_cxl"]) - float(row["cost_dram"])
        worst_total_err = max(worst_total_err, abs(total - expected_total))
        if not close(total, expected_total, rel=1e-9, abs_tol=1e-6):
            total_failures += 1
        rows_checked += 1
    proofs["4_c3_recompute"] = {
        "rows_checked": rows_checked, "calibration_csv": str(CALIB),
        "calibration_sha256": sha256_file(CALIB),
        "formula": "cost = sum_g calib[device][g][dtype] * count_g * bytes_per_value; "
                   "total_score = cost_cxl - cost_dram",
        "cost_recompute_failures": cost_failures, "total_recompute_failures": total_failures,
        "max_rel_cost_error": worst_cost_rel, "max_abs_total_error": worst_total_err,
        "tolerance": "math.isclose(rel_tol=1e-9, abs_tol=1e-6) — abs headroom covers the "
                     "fixed-precision CSV columns",
        "pass": cost_failures == 0 and total_failures == 0,
    }
    proofs["5_dictionary_reported_excluded"] = {
        "dictionary_handling_values": dict(handling_values),
        "rows_with_nonzero_dictionary_count": dict_rows, "total_rows": total_rows,
        "dictionary_count_sum": dict_sum,
        "note": "counters are reported in the trace but never enter the cost formula "
                "(proof 4 recomputes costs from the four gamma counters alone and matches)",
        "pass": set(handling_values) == {"excluded_reported"} and dict_rows > 0,
    }
    proofs["6_counters_nonzero"] = {
        "rows_with_at_least_2_nonzero_counters": counter_rows_ge2, "total_rows": total_rows,
        "fraction": counter_rows_ge2 / total_rows if total_rows else 0.0,
        "gamma_counter_totals": dict(gamma_totals),
        "pass": counter_rows_ge2 > 0 and all(v > 0 for v in gamma_totals.values()),
    }


def proof_7(proofs: dict) -> None:
    """Schedule replay determinism."""
    names = ("smoke_our_full", "smoke_our_full_rerun", "smoke_our_full_block2")
    plan_hash = {n: sha256_file(RUNS / n / "schedule_plan.csv") for n in names}
    plans = {n: read_plan(RUNS / n / "schedule_plan.csv") for n in names}

    def first_diff(a: str, b: str):
        return next((i for i, (x, y) in enumerate(zip(plans[a], plans[b]))
                     if (x["item_id"], x["item_name"]) != (y["item_id"], y["item_name"])), None)

    manifests = {n: list(read_csv_rows(RUNS / n / "schedule_manifest.csv")) for n in names}

    def prefix_len(a: str, b: str):
        n = 0
        for x, y in zip(manifests[a], manifests[b]):
            if (x["submission_index"], x["item_id"], x["item_name"]) != \
               (y["submission_index"], y["item_id"], y["item_name"]):
                break
            n += 1
        return n

    def manifest_matches_plan(n: str):
        for x in manifests[n]:
            p = plans[n][int(x["submission_index"])]
            if x["item_id"] != p["item_id"]:
                return False
        return True

    same_prefix = prefix_len("smoke_our_full", "smoke_our_full_rerun")
    diff_prefix = prefix_len("smoke_our_full", "smoke_our_full_block2")
    proofs["7_schedule_replay"] = {
        "plan_sha256": plan_hash,
        "same_seed_plans_identical": plan_hash["smoke_our_full"] == plan_hash["smoke_our_full_rerun"],
        "same_seed_first_diverging_plan_row": first_diff("smoke_our_full", "smoke_our_full_rerun"),
        "diff_seed_first_diverging_plan_row": first_diff("smoke_our_full", "smoke_our_full_block2"),
        "manifest_lengths": {n: len(manifests[n]) for n in names},
        "same_seed_manifest_common_prefix": same_prefix,
        "diff_seed_manifest_common_prefix": diff_prefix,
        "manifests_are_plan_prefixes": {n: manifest_matches_plan(n) for n in names},
        "pass": (plan_hash["smoke_our_full"] == plan_hash["smoke_our_full_rerun"]
                 and first_diff("smoke_our_full", "smoke_our_full_rerun") is None
                 and first_diff("smoke_our_full", "smoke_our_full_block2") == 0
                 and same_prefix == min(len(manifests["smoke_our_full"]),
                                        len(manifests["smoke_our_full_rerun"]))
                 and diff_prefix == 0
                 and all(manifest_matches_plan(n) for n in names)),
    }


def proof_8(proofs: dict) -> None:
    out = {}
    for name in ("smoke_dram_only", "smoke_cxl_only"):
        data = json.loads((RUNS / name / "numa_residency.json").read_text())
        out[name] = {k: data.get(k) for k in
                     ("target_node", "off_target_pct", "threshold_pct", "verdict", "total_pages")}
    proofs["8_reference_residency"] = {
        "predeclared_threshold_pct": 5.0, "bundles": out,
        "pass": bool(out) and all(v.get("verdict") == "PASS" for v in out.values()),
    }


def proof_9(proofs: dict) -> None:
    path = RUNS / "smoke_our_full_traceon" / "operator_segment_trace.csv"
    levels = Counter()
    measured_missing_reads = 0
    nonmeasured_with_reads = 0
    rows = 0
    enum_ok = True
    for row in read_csv_rows(path):
        rows += 1
        level = row["attribution_level"]
        levels[level] += 1
        if level not in ("measured_per_execution", "counter_delta_concurrent", "plan_reference_only"):
            enum_ok = False
        reads = row.get("actual_reads", "")
        if level == "measured_per_execution" and reads == "":
            measured_missing_reads += 1
        if level != "measured_per_execution" and reads != "":
            nonmeasured_with_reads += 1
    proofs["9_operator_attribution"] = {
        "rows": rows, "level_counts": dict(levels), "level_enum_ok": enum_ok,
        "measured_rows_missing_actual_reads": measured_missing_reads,
        "nonmeasured_rows_with_actual_reads": nonmeasured_with_reads,
        "pass": rows > 0 and enum_ok and measured_missing_reads == 0 and nonmeasured_with_reads == 0,
    }


def proof_10(proofs: dict) -> None:
    env = PHASE_B / "environment"
    required = ["ENVIRONMENT.md", "numactl_H.txt", "lspci_cxl.txt", "cxl_device_detail.txt",
                "platform_identity.txt", "lscpu.txt", "node2_distance.txt", "node2_meminfo.txt"]
    missing = [f for f in required if not (env / f).is_file()]
    numactl = (env / "numactl_H.txt").read_text()
    device = (env / "cxl_device_detail.txt").read_text()
    evidence = {
        "node2_memory_only_no_cpus": "node 2 cpus:\n" in numactl and "node 2 size: 524288 MB" in numactl,
        "cxl_pci_device_present": "1f16:c500" in device and "CXL Memory Device" in device,
        "device_attached_to_numa_node_1": "NUMA node: 1" in device,
    }
    proofs["10_platform_identity"] = {
        "required_files": required, "missing": missing, "evidence": evidence,
        "pass": not missing and all(evidence.values()),
    }


def main() -> int:
    proofs: dict = {}
    for fn in (proof_1, proof_2_3, proof_4_5_6, proof_7, proof_8, proof_9, proof_10):
        fn(proofs)
    all_pass = all(v.get("pass", False) for v in proofs.values() if isinstance(v, dict))
    proofs["ALL_PROOFS_PASS"] = all_pass
    (PHASE_B / "PROOFS.json").write_text(json.dumps(proofs, indent=2, ensure_ascii=False) + "\n")
    lines = ["# Phase B — ten proofs", "", f"**ALL_PROOFS_PASS = {all_pass}**", ""]
    names = {"1_candidate_set": "1  candidate set identical across migration arms",
             "2_strict_fit_budget": "2  strict-fit budget + deterministic tie-break",
             "3_semantic_recompute": "3  semantic score recompute (1e-9)",
             "4_c3_recompute": "4  C3 score recompute from counters + calibration",
             "5_dictionary_reported_excluded": "5  dictionary counters reported, excluded",
             "6_counters_nonzero": "6  ≥2 pattern counters non-zero",
             "7_schedule_replay": "7  schedule replay determinism",
             "8_reference_residency": "8  reference-arm NUMA residency (≤5% off-target)",
             "9_operator_attribution": "9  operator attribution_level / null reads",
             "10_platform_identity": "10 CXL device & platform identity evidence"}
    for key, title in names.items():
        p = proofs[key]
        lines.append(f"## {title} — {'PASS' if p['pass'] else 'FAIL'}")
        for k, v in p.items():
            if k in ("description", "note", "formula", "tolerance"):
                lines.append(f"- {k}: {v}")
            elif isinstance(v, (dict, list)):
                lines.append(f"- `{k}`: `{json.dumps(v, ensure_ascii=False)}`")
            else:
                lines.append(f"- `{k}`: `{v}`")
        lines.append("")
    (PHASE_B / "PROOFS.md").write_text("\n".join(lines))
    print(json.dumps({k: v.get("pass") for k, v in proofs.items() if isinstance(v, dict)}, indent=2))
    print(f"ALL_PROOFS_PASS={all_pass}")
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
