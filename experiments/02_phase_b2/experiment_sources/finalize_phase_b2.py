#!/usr/bin/env python3
"""EDBT v4 Phase B2 finalization.

Run AFTER run_smoke_phase_b2.py, aggregate_calibration_v2.py,
analyze_calibration_sensitivity.py and collect_proofs_phase_b2.py:

1. scans every B2 smoke bundle (exit code, validation_report.json + validated.ok
   stamp, run_config runner/validator SHAs, BASELINE line of cost_c3_greedy
   bundles — bound to the NEW v2 calibration SHA — NUMA residency verdicts),
2. merges PROOFS.json,
3. writes results_edbt_v4/02_phase_b2/PHASE_B2_VERDICT.json,
4. writes results_edbt_v4/02_phase_b2/PHASE_B2_FILES.sha256 covering everything
   under 02_phase_b2/ EXCEPT the manifest itself.
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
PHASE = ROOT / "results_edbt_v4/02_phase_b2"
RUNS = PHASE / "smoke_runs"
CALIB_CSV = PHASE / "calibration/calibration.csv"
EXPECTED_GIT = "eee20419dd7b81b7f82299b8ac3f6ae26b5c9dc33"  # B5 (filled at runtime if HEAD differs)
MANIFEST = "PHASE_B2_FILES.sha256"
VERDICT = "PHASE_B2_VERDICT.json"

BUNDLES = ("smoke_b2_our_full", "smoke_b2_our_freq", "smoke_b2_cost_c3_greedy",
           "smoke_b2_dram_only", "smoke_b2_cxl_only", "smoke_b2_our_full_traceon",
           "smoke_b2_our_freq_traceon", "smoke_b2_cost_c3_greedy_traceon",
           "smoke_b2_our_full_measured", "smoke_b2_our_full_rerun")


def sha256_file(p: Path) -> str:
    return subprocess.check_output(["sha256sum", str(p)], text=True).split()[0]


def bundle_status(name: str, expected_git: str, expected_calib_sha: str) -> dict:
    d = RUNS / name
    status: dict = {"bundle": name}
    if not d.is_dir():
        status["present"] = False
        status["ok"] = False
        return status
    status["present"] = True
    status["exit_code"] = int((d / "exit_code.txt").read_text().strip()) if (d / "exit_code.txt").is_file() else None
    vr = d / "validation_report.json"
    if vr.is_file():
        report = json.loads(vr.read_text())
        status["validator_valid"] = report.get("valid")
        status["validator_errors"] = report.get("errors", [])[:10]
    else:
        status["validator_valid"] = None
    stamp = d / "validated.ok"
    if stamp.is_file():
        s = json.loads(stamp.read_text())
        status["stamp_validator_sha_ok"] = s.get("validator_sha256") is not None
        status["stamp_report_sha_ok"] = s.get("validation_report_sha256") == sha256_file(vr)
    rc = d / "run_config.json"
    if rc.is_file():
        cfg = json.loads(rc.read_text())
        status["config"] = cfg.get("config")
        status["git_hash"] = cfg.get("git_hash")
        status["git_tracked_dirty"] = cfg.get("git_tracked_dirty")
        status["binary_sha256"] = cfg.get("binary_sha256")
        status["schedule_seed"] = cfg.get("schedule_seed")
        if cfg.get("plugin_sha256"):
            status["plugin_sha256"] = cfg.get("plugin_sha256")
    res = d / "numa_residency.json"
    if res.is_file():
        data = json.loads(res.read_text())
        status["numa_off_target_pct"] = data.get("off_target_pct")
        status["numa_verdict"] = data.get("verdict")
    # BASELINE contract for c3 bundles — calibration SHA must be the v2 table
    log = d / "run.log"
    if log.is_file() and "cost_c3_greedy" in name:
        m = re.search(r"BASELINE cost_c3_greedy version=([0-9a-f]{40}) "
                      r"calibration_sha256=([0-9a-f]{64}) counter_set=([\w,]+)", log.read_text())
        if m:
            status["baseline_version_ok"] = m.group(1) == expected_git
            status["baseline_calibration_sha_ok"] = m.group(2) == expected_calib_sha
            status["baseline_counter_set"] = m.group(3)
        else:
            status["baseline_version_ok"] = False
            status["baseline_calibration_sha_ok"] = False
    summary = d / "summary.csv"
    if summary.is_file():
        for line in summary.read_text().splitlines()[1:]:
            parts = line.split(",")
            if len(parts) >= 4:
                status["n_items"] = int(parts[2])
                status["geomean_avg_ms"] = float(parts[3])
    status["ok"] = (status.get("exit_code") == 0
                    and status.get("validator_valid") is True
                    and status.get("stamp_report_sha_ok", False)
                    and status.get("git_hash") == expected_git
                    and status.get("git_tracked_dirty") is False
                    and status.get("baseline_version_ok", True)
                    and status.get("baseline_calibration_sha_ok", True)
                    and status.get("numa_verdict", "PASS") == "PASS")
    return status


def main() -> int:
    expected_git = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"],
                                           text=True).strip()
    expected_calib_sha = sha256_file(CALIB_CSV)

    porcelain = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"],
                                        text=True).splitlines()
    tracked_dirty = any(line and not line.startswith("??") for line in porcelain)

    bundles = [bundle_status(n, expected_git, expected_calib_sha) for n in BUNDLES]
    proofs = json.loads((PHASE / "PROOFS.json").read_text())
    all_bundles_ok = all(b["ok"] for b in bundles)
    verdict = {
        "experiment": "EDBT_V4_PHASE_B2",
        "stage": "B2 corrections (shared enumerator, all-policy counters, measured "
                 "diagnostic, calibration redo, sensitivity, documentation) — NOT the "
                 "formal E12/E13 runs",
        "git_head": expected_git,
        "git_tracked_dirty_at_finalize": tracked_dirty,
        "binary_sha256": bundles[0].get("binary_sha256") if bundles else None,
        "plugin_sha256": bundles[0].get("plugin_sha256") if bundles else None,
        "dram_budget_bytes": 3725884160,
        "off_target_page_threshold_pct": 5.0,
        "calibration_v2_sha256": expected_calib_sha,
        "bundles": bundles,
        "all_bundles_ok": all_bundles_ok,
        "proofs_all_pass": proofs.get("ALL_PROOFS_PASS"),
        "proofs": {p["name"]: p["ok"] for p in proofs.get("proofs", [])},
        "next_step": "STOP and wait for human review; do NOT start the formal campaign",
    }
    verdict["PHASE_B2_PASS"] = bool(all_bundles_ok and proofs.get("ALL_PROOFS_PASS") and not tracked_dirty)
    (PHASE / VERDICT).write_text(json.dumps(verdict, indent=2, ensure_ascii=False) + "\n")

    files = sorted(p for p in PHASE.rglob("*") if p.is_file() and p.name != MANIFEST)
    lines = []
    for p in files:
        h = hashlib.sha256()
        with p.open("rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        lines.append(f"{h.hexdigest()}  {p.relative_to(PHASE)}")
    (PHASE / MANIFEST).write_text("\n".join(lines) + "\n")
    print(f"{VERDICT}: PHASE_B2_PASS={verdict['PHASE_B2_PASS']}")
    print(f"{MANIFEST}: {len(lines)} files")
    for b in bundles:
        print(f"  {b['bundle']}: ok={b.get('ok')} exit={b.get('exit_code')} "
              f"validator={b.get('validator_valid')} stamp={b.get('stamp_report_sha_ok')}")
    return 0 if verdict["PHASE_B2_PASS"] else 1


if __name__ == "__main__":
    sys.exit(main())
