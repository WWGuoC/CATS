#!/usr/bin/env python3
"""EDBT v4 Phase B2 mechanism proxy builder (B2-4, downstream half).

The B2-4 trace emits CUMULATIVE typed counters per (cycle, segment) for every
policy. This script turns them into:

  1. per_cycle_segment_deltas.csv.gz — the per-cycle delta per segment
     (counter[j] - counter[i] for consecutive snapshots i -> j of the same
     segment, clamped at 0), annotated with the tier IN EFFECT during that
     interval (the post-decision tier of snapshot i).
  2. tiered_logical_access_proxy.csv — logical access events aggregated by
     effective tier and access pattern.

IMPORTANT SEMANTICS (B2-4 wording):
  * These are LOGICAL ACCESS PROXY numbers derived from Hyrise per-segment
    access counters. They are NOT physical CXL/DRAM traffic measurements —
    no byte counts on the interconnect are claimed, pages may be prefetched,
    counters count operator-level touches, and dictionary events are separate
    dictionary-structure accesses.
  * Interval attribution: the placement state during interval (i, j] is the
    state AFTER cycle i's decisions, i.e. target_tier when the row records a
    move, else source_tier ("UNCHANGED"/"keep" rows).
  * Coverage: accesses before the FIRST daemon snapshot and after the LAST
    snapshot are not attributable to any interval and are excluded — proxy
    totals are strictly less than the final cumulative counters (documented
    under-coverage, no inflation).

Usage:
  python3 build_mechanism_proxy.py \
      --bundles smoke_b2_our_full_traceon smoke_b2_our_freq_traceon \
                smoke_b2_cost_c3_greedy_traceon \
      [--smoke-root results_edbt_v4/02_phase_b2/smoke_runs] \
      [--outdir   results_edbt_v4/02_phase_b2/mechanism_proxy]
"""
from __future__ import annotations

import argparse
import csv
import gzip
import hashlib
import json
import shutil
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
GAMMAS = ("sequential", "monotonic", "random", "point", "dictionary")
COUNTER_COLS = ("sequential_count", "monotonic_count", "random_count",
                "point_count", "dictionary_count")


def sha256_file(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def git_hash() -> str:
    return subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                   text=True).strip()


def counts_of(row: dict) -> tuple[int, ...]:
    out = []
    for c in COUNTER_COLS:
        v = (row.get(c) or "").strip()
        out.append(int(v) if v else 0)
    return tuple(out)


def post_tier_of(row: dict) -> str:
    # Tier in effect after this cycle's decision: a recorded move carries the
    # new tier in target_tier; "UNCHANGED"/"keep" rows keep source_tier.
    tgt = (row.get("target_tier") or "").strip()
    if tgt in ("DRAM", "CXL"):
        return tgt
    return (row.get("source_tier") or "").strip() or "UNKNOWN"


def process_bundle(bundle_dir: Path, outdir: Path) -> dict:
    trace = bundle_dir / "decision_trace.csv"
    if not trace.exists():
        alt = bundle_dir / "benchmark_decision_trace.csv"
        if not alt.exists():
            raise FileNotFoundError(f"no decision trace in {bundle_dir}")
        trace = alt
    run_id = bundle_dir.name
    cfg_name = json.loads((bundle_dir / "run_config.json").read_text()).get("config", run_id)

    # Pass 1: collect per-segment snapshot sequences (cycle order as emitted).
    per_seg: dict[tuple, list[dict]] = defaultdict(list)
    n_rows = 0
    with open(trace, newline="") as f:
        for row in csv.DictReader(f):
            n_rows += 1
            key = (row["table_name"], int(row["chunk_id"]), int(row["column_id"]))
            per_seg[key].append(row)

    # Pass 2: consecutive-snapshot deltas.
    deltas_path = outdir / f"per_cycle_segment_deltas_{run_id}.csv.gz"
    agg: dict[tuple[str, str], int] = defaultdict(int)  # (tier, gamma) -> events
    n_delta_rows = 0
    covered_total = 0
    final_cumulative = [0] * len(GAMMAS)
    with gzip.open(deltas_path, "wt", newline="") as f:
        w = csv.writer(f)
        w.writerow(["run_id", "config", "table_name", "chunk_id", "column_id",
                    "cycle_from", "cycle_to", "phase", "tier_effective",
                    "delta_sequential", "delta_monotonic", "delta_random",
                    "delta_point", "delta_dictionary", "delta_total",
                    "attribution_level"])
        for key, rows in per_seg.items():
            rows.sort(key=lambda r: int(r["cycle_id"]))  # safety; emitted in order
            final = counts_of(rows[-1])
            final_cumulative = [a + b for a, b in zip(final_cumulative, final)]
            for i, j in zip(rows, rows[1:]):
                ci, cj = counts_of(i), counts_of(j)
                d = tuple(max(0, b - a) for a, b in zip(ci, cj))
                tier = post_tier_of(i)
                total = sum(d)
                w.writerow([run_id, cfg_name, key[0], key[1], key[2],
                            i["cycle_id"], j["cycle_id"], i.get("phase", ""),
                            tier, *d, total, "interval_logical_proxy"])
                n_delta_rows += 1
                covered_total += total
                for g, v in zip(GAMMAS, d):
                    agg[(tier, g)] += v
            # the last snapshot of each segment has no successor interval —
            # its (yet unobserved) events simply never enter the proxy

    return {
        "bundle": run_id, "config": cfg_name, "trace_rows": n_rows,
        "segments": len(per_seg), "delta_rows": n_delta_rows,
        "deltas_gz": deltas_path.name, "deltas_gz_sha256": sha256_file(deltas_path),
        "final_cumulative_total": sum(final_cumulative),
        "covered_logical_events": covered_total,
        "coverage_note": "events before first / after last snapshot excluded",
        "aggregate": {f"{tier}|{gamma}": v for (tier, gamma), v in sorted(agg.items())},
        "_agg": agg, "_final": final_cumulative,
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bundles", nargs="+", required=True)
    ap.add_argument("--smoke-root", default=str(ROOT / "results_edbt_v4/02_phase_b2/smoke_runs"))
    ap.add_argument("--outdir", default=str(ROOT / "results_edbt_v4/02_phase_b2/mechanism_proxy"))
    args = ap.parse_args()
    smoke_root = Path(args.smoke_root)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    results = []
    proxy_rows = []
    for name in args.bundles:
        print(f"[PROXY] {name}", flush=True)
        r = process_bundle(smoke_root / name, outdir)
        agg, final = r.pop("_agg"), r.pop("_final")
        results.append(r)
        for (tier, gamma), v in sorted(agg.items()):
            proxy_rows.append([r["bundle"], r["config"], tier, gamma,
                               v, "logical_access_events"])

    with open(outdir / "tiered_logical_access_proxy.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["run_id", "config", "tier_effective", "access_pattern",
                    "logical_access_events", "unit_note"])
        w.writerows(proxy_rows)

    report = {
        "experiment": "EDBT_V4_PHASE_B2_MECHANISM_PROXY",
        "git_head": git_hash(),
        "script": str(Path(__file__).resolve()),
        "script_sha256": sha256_file(Path(__file__).resolve()),
        "semantics": [
            "LOGICAL ACCESS PROXY from Hyrise per-segment typed access counters;",
            "NOT physical CXL/DRAM interconnect traffic (no bytes claimed)",
            "interval (i,j] attributed to the post-decision tier of snapshot i",
            "pre-first-snapshot and post-last-snapshot events excluded (documented",
            "under-coverage: proxy totals < final cumulative counters)",
        ],
        "bundles": results,
    }
    (outdir / "mechanism_proxy_report.json").write_text(json.dumps(report, indent=2) + "\n")
    shutil.copy2(Path(__file__).resolve(), outdir / "build_mechanism_proxy.py.snapshot")

    # Human-readable summary
    lines = ["# B2-4 mechanism proxy — tiered logical access events", "",
             "> Logical access proxy from per-segment typed counters. NOT physical",
             "> CXL/DRAM traffic. Interval attribution: tier in effect after cycle i",
             "> governs events counted between snapshots i and j.", ""]
    for r in results:
        lines.append(f"## {r['bundle']} ({r['config']})")
        lines.append(f"- trace rows {r['trace_rows']}, segments {r['segments']}, "
                     f"delta rows {r['delta_rows']}")
        lines.append(f"- covered logical events {r['covered_logical_events']} of final "
                     f"cumulative {r['final_cumulative_total']} "
                     f"({100.0 * r['covered_logical_events'] / max(1, r['final_cumulative_total']):.1f}%)")
        tiers = defaultdict(dict)
        for k, v in r["aggregate"].items():
            t, g = k.split("|")
            tiers[t][g] = v
        for t in sorted(tiers):
            tot = sum(tiers[t].values())
            lines.append(f"- tier {t}: {tot} events — " +
                         ", ".join(f"{g}={tiers[t][g]}" for g in GAMMAS if g in tiers[t]))
        lines.append("")
    (outdir / "README.md").write_text("\n".join(lines) + "\n")
    print(json.dumps({k: v for k, v in report.items() if k != "bundles"}, indent=2))
    for r in results:
        print(f"  {r['bundle']}: {r['delta_rows']} delta rows, "
              f"{r['covered_logical_events']} covered events")
    return 0


if __name__ == "__main__":
    sys.exit(main())
