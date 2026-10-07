#!/usr/bin/env python3
"""EDBT v4 Phase B2 (B2-7 + B2-8): calibration & Dictionary placement sensitivity.

Inputs:
  --trace <decision_trace.csv>   a cost_c3_greedy trace-on bundle's decision trace
                                 (carries per-cycle raw 5-pattern counters, dtype,
                                 bpv, data_volume_bytes)
  --stats <calibration_stats.csv> aggregate_calibration_v2.py output
  --cycle last|<n>               which daemon cycle's cumulative counters to use
                                 as the workload snapshot (default: last)

Analyses:
  B2-7  placement under median calibration vs UNCERTAINTY ENDPOINTS
        (all cells at ci95_low / all at ci95_high). Benefit recomputed from raw
        counters; the shared strict-fit mapper (score desc, then
        table/chunk/col asc; select only if bytes <= remaining budget,
        budget = 3725884160) is re-run per variant. Report segment-count and
        byte-weighted Jaccard vs the median placement.
  B2-8  Dictionary event share (cumulative + per-cycle delta) and a DOCUMENTED
        placement sensitivity that maps Dictionary accesses to a defensible
        calibrated cost (point / random), reported against the published
        four-pattern arm (dictionary excluded). The four-pattern arm itself is
        NOT changed here; both outcomes are reported.

Outputs (in --outdir):
  sensitivity_calibration.json / .csv   B2-7 endpoints
  sensitivity_dictionary.json / .csv    B2-8 share + mapping variants
"""

import argparse
import csv
import json
from collections import defaultdict
from pathlib import Path

DRAM_BUDGET_BYTES = 3725884160
GAMMAS = ["sequential", "monotonic", "random", "point"]


def load_stats(path: Path) -> dict:
    """(device, gamma, dtype) -> stats row."""
    out = {}
    for r in csv.DictReader(open(path)):
        out[(r["device"], r["gamma"], r["data_type"])] = r
    return out


def calibration_value(stats: dict, variant: str) -> dict:
    """(device, gamma, dtype) -> ns_per_access_per_byte under the variant."""
    key = {"median": "median_ns_per_access_per_byte",
           "lo": "ci95_low", "hi": "ci95_high"}[variant]
    return {cell: float(row[key]) for cell, row in stats.items()}


def benefit(counts: dict, bpv: int, dtype: str, calib: dict) -> float:
    cost_dram = sum(calib[("dram", g, dtype)] * counts[g] * bpv for g in GAMMAS)
    cost_cxl = sum(calib[("cxl", g, dtype)] * counts[g] * bpv for g in GAMMAS)
    return cost_cxl - cost_dram


def strict_fit(segments: list[dict], budget: int = DRAM_BUDGET_BYTES) -> set[int]:
    """Shared deterministic strict-fit mapper (commit B2 semantics):
    comparator = score desc, then table name, chunk id, column id ascending;
    a segment is selected only when its bytes fit the remaining budget."""
    order = sorted(range(len(segments)),
                   key=lambda i: (-segments[i]["score"], segments[i]["table"],
                                  segments[i]["chunk"], segments[i]["column"]))
    remaining = budget
    selected = set()
    for i in order:
        b = segments[i]["bytes"]
        if b <= remaining:
            selected.add(i)
            remaining -= b
    return selected


def jaccard(a: set, b: set) -> float:
    return len(a & b) / len(a | b) if (a | b) else 1.0


def byte_weighted_jaccard(segments: list[dict], a: set, b: set) -> float:
    inter = sum(segments[i]["bytes"] for i in a & b)
    union = sum(segments[i]["bytes"] for i in a | b)
    return inter / union if union else 1.0


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--trace", required=True)
    ap.add_argument("--stats", required=True)
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--cycle", default="last")
    args = ap.parse_args()
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    rows = list(csv.DictReader(open(args.trace)))
    assert rows, "empty decision trace"
    cycles = sorted({int(r["cycle_id"]) for r in rows})
    cycle = cycles[-1] if args.cycle == "last" else int(args.cycle)
    assert cycle in cycles, f"cycle {cycle} not in {cycles}"
    snap = [r for r in rows if int(r["cycle_id"]) == cycle and r["policy"] == "cost_c3_greedy"]
    assert snap, f"no cost_c3_greedy rows in cycle {cycle}"

    # DTYPE normalization: trace data_type is the magic_enum-lower column type
    # (int/long/float/double/string/null); calibration keys are the loader grid.
    DTYPE_TO_CALIB = {"int": "int32", "long": "int64", "float": "float32",
                      "double": "float64", "string": "string16", "null": "int64"}
    CALIB_BYTES = {"int32": 4, "int64": 8, "float32": 4, "float64": 8, "string16": 16}

    segments = []
    for r in snap:
        dtype = DTYPE_TO_CALIB[r["data_type"]]
        bpv = int(float(r["bytes_per_value"]))
        assert bpv == CALIB_BYTES[dtype], (r["data_type"], bpv, dtype)
        counts = {"sequential": int(r["sequential_count"]),
                  "monotonic": int(r["monotonic_count"]),
                  "random": int(r["random_count"]),
                  "point": int(r["point_count"])}
        segments.append(dict(table=r["table_name"], chunk=int(r["chunk_id"]),
                             column=int(r["column_id"]), dtype=dtype, bpv=bpv,
                             counts=counts, dictionary=int(r["dictionary_count"]),
                             bytes=int(r["data_volume_bytes"])))

    stats = load_stats(Path(args.stats))

    # ── B2-7: placement under median vs uncertainty endpoints ──────────────
    cal_results = {}
    placements = {}
    for variant in ("median", "lo", "hi"):
        calib = calibration_value(stats, variant)
        for seg in segments:
            seg["score"] = benefit(seg["counts"], seg["bpv"], seg["dtype"], calib)
        placements[variant] = strict_fit(segments)
        cal_results[variant] = dict(
            selected_segments=len(placements[variant]),
            selected_bytes=sum(segments[i]["bytes"] for i in placements[variant]),
            benefit_total=sum(segments[i]["score"] for i in placements[variant]))

    b27 = dict(
        cycle=cycle,
        segments=len(segments),
        budget=DRAM_BUDGET_BYTES,
        variants=cal_results,
        jaccard_segment={v: jaccard(placements["median"], placements[v]) for v in ("lo", "hi")},
        jaccard_byte_weighted={v: byte_weighted_jaccard(segments, placements["median"], placements[v])
                               for v in ("lo", "hi")})

    # ── B2-8: Dictionary event share + mapped-cost placement sensitivity ───
    tot = {g: sum(s["counts"][g] for s in segments) for g in GAMMAS}
    dict_total = sum(s["dictionary"] for s in segments)
    all_events = sum(tot.values()) + dict_total
    share_cumulative = dict_total / all_events if all_events else 0.0

    # per-cycle delta share: dictionary delta vs total delta across consecutive cycles
    dict_deltas, total_deltas = [], []
    prev: dict[tuple, tuple] = {}
    for r in rows:
        if r["policy"] != "cost_c3_greedy":
            continue
        key = (r["table_name"], int(r["chunk_id"]), int(r["column_id"]))
        cur = (int(r["sequential_count"]), int(r["monotonic_count"]), int(r["random_count"]),
               int(r["point_count"]), int(r["dictionary_count"]))
        if key in prev:
            d4 = sum(c - p for c, p in zip(cur[:4], prev[key][:4]))
            dd = cur[4] - prev[key][4]
            if d4 + dd > 0:
                dict_deltas.append(dd)
                total_deltas.append(d4 + dd)
        prev[key] = cur
    dict_delta_sum, total_delta_sum = sum(dict_deltas), sum(total_deltas)
    share_delta = dict_delta_sum / total_delta_sum if total_delta_sum else 0.0

    # mapped-cost variants (documented): Dictionary -> point cost or random cost
    # on BOTH devices, added to the four-pattern benefit. The published arm
    # (excluded_reported) stays the reference.
    calib_median = calibration_value(stats, "median")
    dict_results = {}
    dict_placements = {}
    for mapping in ("excluded_reported", "point", "random"):
        for seg in segments:
            extra = {"excluded_reported": 0, "point": seg["dictionary"], "random": seg["dictionary"]}[mapping]
            counts = dict(seg["counts"])
            if mapping in ("point", "random"):
                counts[mapping] += extra
            seg["score"] = benefit(counts, seg["bpv"], seg["dtype"], calib_median)
        dict_placements[mapping] = strict_fit(segments)
        dict_results[mapping] = dict(
            selected_segments=len(dict_placements[mapping]),
            selected_bytes=sum(segments[i]["bytes"] for i in dict_placements[mapping]))

    b28 = dict(
        cycle=cycle,
        dictionary_events_cumulative=dict_total,
        four_pattern_events_cumulative=sum(tot.values()),
        dictionary_share_cumulative=share_cumulative,
        dictionary_events_delta=dict_delta_sum,
        total_events_delta=total_delta_sum,
        dictionary_share_delta=share_delta,
        mapping_variants=dict_results,
        jaccard_segment_vs_excluded={m: jaccard(dict_placements["excluded_reported"], dict_placements[m])
                                     for m in ("point", "random")},
        jaccard_byte_weighted_vs_excluded={m: byte_weighted_jaccard(
            segments, dict_placements["excluded_reported"], dict_placements[m])
            for m in ("point", "random")})

    json.dump(dict(b27_calibration_sensitivity=b27, b28_dictionary_sensitivity=b28,
                   inputs=dict(trace=str(args.trace), stats=str(args.stats), cycle=cycle)),
              open(outdir / "sensitivity_results.json", "w"), indent=2)

    with open(outdir / "sensitivity_calibration.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["variant", "selected_segments", "selected_bytes",
                    "jaccard_segment_vs_median", "jaccard_byte_weighted_vs_median"])
        for v in ("median", "lo", "hi"):
            r = cal_results[v]
            w.writerow([v, r["selected_segments"], r["selected_bytes"],
                        1.0 if v == "median" else b27["jaccard_segment"][v],
                        1.0 if v == "median" else b27["jaccard_byte_weighted"][v]])
    with open(outdir / "sensitivity_dictionary.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["metric", "value"])
        w.writerow(["dictionary_share_cumulative", share_cumulative])
        w.writerow(["dictionary_share_per_cycle_delta", share_delta])
        w.writerow(["dictionary_events_cumulative", dict_total])
        w.writerow(["four_pattern_events_cumulative", sum(tot.values())])
        for m in ("excluded_reported", "point", "random"):
            w.writerow([f"selected_segments[{m}]", dict_results[m]["selected_segments"]])
            w.writerow([f"selected_bytes[{m}]", dict_results[m]["selected_bytes"]])
            if m != "excluded_reported":
                w.writerow([f"jaccard_segment_vs_excluded[{m}]", b28["jaccard_segment_vs_excluded"][m]])
                w.writerow([f"jaccard_byte_weighted_vs_excluded[{m}]",
                            b28["jaccard_byte_weighted_vs_excluded"][m]])

    print(json.dumps(dict(b27=b27["jaccard_segment"], b27_bytes=b27["jaccard_byte_weighted"],
                          dict_share_cum=share_cumulative, dict_share_delta=share_delta,
                          b28=b28["jaccard_segment_vs_excluded"]), indent=2))


if __name__ == "__main__":
    main()
