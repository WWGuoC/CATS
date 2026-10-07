#!/usr/bin/env python3
"""Apply the predeclared 3% trace-overhead gate to paired Our-full runs."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path


def first(obj: dict, names: tuple[str, ...]):
    for name in names:
        if obj.get(name) not in (None, ""):
            return obj[name]
    return None


def bool_value(value) -> bool:
    return str(value).strip().lower() in {"true", "1", "yes", "on"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--threshold-pct", type=float, default=3.0)
    args = parser.parse_args()
    pairs: dict[str, dict[bool, float]] = {}
    hashes: dict[str, dict[bool, str | None]] = {}
    for summary_path in sorted(args.root.resolve().rglob("summary.csv")):
        run_dir = summary_path.parent
        if not (run_dir / "validated.ok").is_file():
            continue
        config = json.loads((run_dir / "run_config.json").read_text(encoding="utf-8"))
        if first(config, ("config", "configuration")) != "our_full":
            continue
        with summary_path.open(newline="", encoding="utf-8") as handle:
            row = next(csv.DictReader(handle))
        rep = str(first(config, ("rep", "repetition", "paired_block")))
        traced = bool_value(config.get("trace", False))
        metric = float(first(row, ("geomean_avg_ms", "geomean_ms")))
        pairs.setdefault(rep, {})[traced] = metric
        hashes.setdefault(rep, {})[traced] = first(config, ("query_schedule_sha256", "schedule_sha256", "query_order_sha256"))

    complete = sorted((rep, values) for rep, values in pairs.items() if set(values) == {False, True})
    ratios = []
    hash_match = True
    raw = []
    for rep, values in complete:
        overhead = 100.0 * (values[True] / values[False] - 1.0)
        ratios.append(math.log(values[True] / values[False]))
        left, right = hashes[rep][False], hashes[rep][True]
        if left and right and left != right:
            hash_match = False
        raw.append({"rep": rep, "trace_off_ms": values[False], "trace_on_ms": values[True], "overhead_pct": overhead})
    aggregate = 100.0 * (math.exp(sum(ratios) / len(ratios)) - 1.0) if ratios else None
    passed = len(complete) >= 3 and hash_match and aggregate is not None and abs(aggregate) <= args.threshold_pct
    report = {
        "paired_runs": len(complete), "paired_schedule_hashes_match": hash_match,
        "aggregate_geometric_overhead_pct": aggregate,
        "absolute_threshold_pct": args.threshold_pct,
        "gate": "PASS" if passed else "FAIL",
        "interpretation": "PASS permits diagnostic mechanism use only; traced runs never replace trace-off headline runs",
        "pairs": raw,
    }
    args.output.resolve().parent.mkdir(parents=True, exist_ok=True)
    args.output.resolve().write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    return 0 if passed else 2


if __name__ == "__main__":
    raise SystemExit(main())
