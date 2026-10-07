#!/usr/bin/env python3
"""EDBT v4 Phase B: assemble schedule_tests/ artifacts from the smoke bundles.

Produces:
  schedule_tests/replay_report.json        — full determinism analysis
  schedule_tests/plan_head_comparison.csv  — first N plan rows, three arms side by side
  schedule_tests/manifest_head_comparison.csv — same for the manifests
  schedule_tests/README.md is hand-written; this script only emits the data files.
"""
from __future__ import annotations

import csv
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
RUNS = ROOT / "results_edbt_v4/01_phase_b/smoke_runs"
OUT = ROOT / "results_edbt_v4/01_phase_b/schedule_tests"
ARMS = {"seed101_a": "smoke_our_full", "seed101_b": "smoke_our_full_rerun",
        "seed202": "smoke_our_full_block2"}
N_HEAD = 25


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_plan(p: Path) -> list[dict]:
    lines = p.read_text().splitlines()
    assert lines[0] == "seed,item_count,plan_length"
    return list(csv.DictReader("\n".join(lines[2:]).splitlines(keepends=True)))


def read_manifest(p: Path) -> list[dict]:
    with p.open(newline="") as f:
        return list(csv.DictReader(f))


def main() -> int:
    OUT.mkdir(parents=True, exist_ok=True)
    plans = {k: read_plan(RUNS / v / "schedule_plan.csv") for k, v in ARMS.items()}
    manifests = {k: read_manifest(RUNS / v / "schedule_manifest.csv") for k, v in ARMS.items()}

    def first_plan_diff(a, b):
        return next((i for i, (x, y) in enumerate(zip(plans[a], plans[b]))
                     if (x["item_id"], x["item_name"]) != (y["item_id"], y["item_name"])), None)

    def prefix(a, b):
        n = 0
        for x, y in zip(manifests[a], manifests[b]):
            if (x["submission_index"], x["item_id"], x["item_name"]) != \
               (y["submission_index"], y["item_id"], y["item_name"]):
                break
            n += 1
        return n

    def plan_prefix_ok(k):
        return all(x["item_id"] == plans[k][int(x["submission_index"])]["item_id"]
                   for x in manifests[k])

    report = {
        "contract": "same --schedule_seed => byte-identical plan + manifest that is an "
                    "exact prefix of it; different seed => fork at submission 0",
        "plan_sha256": {k: sha256_file(RUNS / v / "schedule_plan.csv") for k, v in ARMS.items()},
        "plan_meta": {k: (RUNS / v / "schedule_plan.csv").read_text().splitlines()[1]
                      for k, v in ARMS.items()},
        "plan_length_rows": {k: len(v) for k, v in plans.items()},
        "manifest_rows": {k: len(v) for k, v in manifests.items()},
        "same_seed_plan_first_diverging_row": first_plan_diff("seed101_a", "seed101_b"),
        "diff_seed_plan_first_diverging_row": first_plan_diff("seed101_a", "seed202"),
        "same_seed_manifest_common_prefix_rows": prefix("seed101_a", "seed101_b"),
        "diff_seed_manifest_common_prefix_rows": prefix("seed101_a", "seed202"),
        "manifest_is_plan_prefix": {k: plan_prefix_ok(k) for k in ARMS},
        "verdict": "PASS" if (
            sha256_file(RUNS / ARMS["seed101_a"] / "schedule_plan.csv")
            == sha256_file(RUNS / ARMS["seed101_b"] / "schedule_plan.csv")
            and first_plan_diff("seed101_a", "seed202") == 0
            and prefix("seed101_a", "seed101_b") == min(len(manifests["seed101_a"]),
                                                        len(manifests["seed101_b"]))
            and prefix("seed101_a", "seed202") == 0
            and all(plan_prefix_ok(k) for k in ARMS)) else "FAIL",
    }
    (OUT / "replay_report.json").write_text(json.dumps(report, indent=2) + "\n")

    keys = list(ARMS)
    with (OUT / "plan_head_comparison.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["submission_index"] + [f"{k}_item_id" for k in keys]
                   + [f"{k}_item_name" for k in keys])
        for i in range(N_HEAD):
            w.writerow([i] + [plans[k][i]["item_id"] for k in keys]
                       + [plans[k][i]["item_name"] for k in keys])
    with (OUT / "manifest_head_comparison.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["submission_index"] + [f"{k}_item_id" for k in keys]
                   + [f"{k}_item_name" for k in keys] + [f"{k}_scheduled_unix_ns" for k in keys])
        for i in range(N_HEAD):
            w.writerow([i] + [manifests[k][i]["item_id"] for k in keys]
                       + [manifests[k][i]["item_name"] for k in keys]
                       + [manifests[k][i]["scheduled_unix_ns"] for k in keys])
    print(json.dumps({k: report[k] for k in
                      ("same_seed_plan_first_diverging_row", "diff_seed_plan_first_diverging_row",
                       "same_seed_manifest_common_prefix_rows", "diff_seed_manifest_common_prefix_rows",
                       "manifest_is_plan_prefix", "verdict")}, indent=2))
    return 0 if report["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
