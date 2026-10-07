#!/usr/bin/env python3
"""Analyze validated paired repetitions for E12 without SciPy or pandas."""

from __future__ import annotations

import argparse
import csv
import itertools
import json
import math
from collections import defaultdict
from pathlib import Path

import numpy as np


COMPARISONS = (
    ("our_full", "our_freq"),
    ("our_full", "cost_c3_greedy"),
    ("our_full", "dram_only"),
    ("our_full", "cxl_only"),
)


def first_value(objects: list[dict], keys: tuple[str, ...]):
    for obj in objects:
        for key in keys:
            if obj.get(key) not in (None, ""):
                return obj[key]
    return None


def load_runs(root: Path) -> list[dict]:
    runs = []
    for summary_path in sorted(root.rglob("summary.csv")):
        run_dir = summary_path.parent
        if not (run_dir / "validated.ok").is_file():
            continue
        with summary_path.open(newline="", encoding="utf-8") as handle:
            rows = list(csv.DictReader(handle))
        if len(rows) != 1:
            raise ValueError(f"expected one row in {summary_path}, got {len(rows)}")
        summary = rows[0]
        config_data = json.loads((run_dir / "run_config.json").read_text(encoding="utf-8"))
        config = first_value([summary, config_data], ("config", "configuration"))
        rep = first_value([summary, config_data], ("rep", "repetition", "repetition_id", "paired_block"))
        metric = first_value([summary], ("geomean_avg_ms", "geomean_ms"))
        if config is None or rep is None or metric is None:
            raise ValueError(f"missing config/rep/geomean in {run_dir}")
        metric_value = float(metric)
        if not math.isfinite(metric_value) or metric_value <= 0:
            raise ValueError(f"non-positive/non-finite geomean in {run_dir}")
        schedule_hash = first_value([config_data], ("query_schedule_sha256", "schedule_sha256", "query_order_sha256"))
        runs.append({
            "config": str(config), "rep": str(rep), "geomean_avg_ms": metric_value,
            "schedule_hash": schedule_hash, "run_dir": str(run_dir.resolve()),
        })
    if not runs:
        raise ValueError(f"no validated runs found below {root}")
    seen = set()
    for run in runs:
        key = (run["config"], run["rep"])
        if key in seen:
            raise ValueError(f"duplicate validated config/rep: {key}")
        seen.add(key)
    return runs


def bootstrap_ci(log_diffs: np.ndarray, seed: int, samples: int = 10000) -> tuple[float, float]:
    rng = np.random.default_rng(seed)
    means = np.empty(samples)
    n = len(log_diffs)
    for i in range(samples):
        means[i] = np.mean(log_diffs[rng.integers(0, n, n)])
    lo, hi = np.quantile(means, [0.025, 0.975])
    return 100.0 * (math.exp(float(lo)) - 1.0), 100.0 * (math.exp(float(hi)) - 1.0)


def sign_flip_p(log_diffs: np.ndarray, seed: int) -> tuple[float, str]:
    observed = abs(float(np.mean(log_diffs)))
    n = len(log_diffs)
    if n <= 20:
        stats = []
        for signs in itertools.product((-1.0, 1.0), repeat=n):
            stats.append(abs(float(np.mean(log_diffs * np.asarray(signs)))))
        p = sum(stat >= observed - 1e-15 for stat in stats) / len(stats)
        return p, f"exact_sign_flip_{2**n}"
    rng = np.random.default_rng(seed)
    samples = 100000
    exceed = 0
    for _ in range(samples):
        signs = rng.choice((-1.0, 1.0), size=n)
        exceed += abs(float(np.mean(log_diffs * signs))) >= observed - 1e-15
    return (exceed + 1) / (samples + 1), f"monte_carlo_sign_flip_{samples}"


def compare(a: str, b: str, lookup: dict, seed: int) -> dict:
    reps = sorted(set(lookup[a]) & set(lookup[b]), key=lambda x: (len(x), x))
    if not reps:
        return {"comparison": f"{a}_vs_{b}", "error": "no paired repetitions"}
    hashes_match = True
    pairs = []
    for rep in reps:
        left, right = lookup[a][rep], lookup[b][rep]
        if left["schedule_hash"] and right["schedule_hash"] and left["schedule_hash"] != right["schedule_hash"]:
            hashes_match = False
        pairs.append((left["geomean_avg_ms"], right["geomean_avg_ms"]))
    values = np.asarray(pairs, dtype=float)
    log_diffs = np.log(values[:, 0] / values[:, 1])
    estimate = 100.0 * (math.exp(float(np.mean(log_diffs))) - 1.0)
    ci_low, ci_high = bootstrap_ci(log_diffs, seed)
    p_value, p_method = sign_flip_p(log_diffs, seed + 1)
    return {
        "comparison": f"{a}_vs_{b}",
        "metric": "paired_geometric_mean_latency_ratio",
        "interpretation": "negative effect_pct means the first configuration is faster",
        "n_pairs": len(reps),
        "paired_blocks": reps,
        "all_available_schedule_hashes_match": hashes_match,
        "effect_pct": estimate,
        "bootstrap_95_ci_pct": [ci_low, ci_high],
        "randomization_p_value_two_sided": p_value,
        "p_value_method": p_method,
        "raw_pairs_ms": [{a: float(x), b: float(y)} for x, y in pairs],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seed", type=int, default=20270910)
    args = parser.parse_args()
    runs = load_runs(args.root.resolve())
    lookup: dict[str, dict[str, dict]] = defaultdict(dict)
    for run in runs:
        lookup[run["config"]][run["rep"]] = run

    results = [compare(a, b, lookup, args.seed + i * 100) for i, (a, b) in enumerate(COMPARISONS)]
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    (output_dir / "core_statistics.json").write_text(
        json.dumps({"seed": args.seed, "validated_runs": len(runs), "comparisons": results}, indent=2) + "\n",
        encoding="utf-8",
    )
    with (output_dir / "validated_run_metrics.csv").open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(runs[0]))
        writer.writeheader()
        writer.writerows(runs)
    print(json.dumps({"validated_runs": len(runs), "output_dir": str(output_dir), "comparisons": results}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
