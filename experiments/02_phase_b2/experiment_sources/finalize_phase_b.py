#!/usr/bin/env python3
"""EDBT v4 Phase B finalization.

Run AFTER run_smoke_phase_b.py and collect_proofs_phase_b.py:

1. scans every smoke bundle (exit code, validation_report.json, run_config,
   BASELINE line of cost_c3_greedy bundles, NUMA residency verdicts),
2. merges PROOFS.json,
3. writes results_edbt_v4/01_phase_b/SMOKE_VERDICT.json,
4. writes results_edbt_v4/01_phase_b/PHASE_B_FILES.sha256 covering everything
   under 01_phase_b/ EXCEPT the manifest itself (gate fix 1 applies to this
   manifest too).
"""
from __future__ import annotations

import hashlib
import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
PHASE_B = ROOT / "results_edbt_v4/01_phase_b"
RUNS = PHASE_B / "smoke_runs"
EXPECTED_BASELINE_SHA = "24cdfedff80a9ce962136979060a2fc1c293e4300cd61685a14a39b92806f40a"
EXPECTED_GIT = "fab61b2fee237323833be3fa333054ce669fbff5"
MANIFEST = "PHASE_B_FILES.sha256"
VERDICT = "SMOKE_VERDICT.json"

BUNDLES = ("smoke_our_full", "smoke_our_freq", "smoke_cost_c3_greedy",
           "smoke_dram_only", "smoke_cxl_only", "smoke_our_full_traceon",
           "smoke_our_freq_traceon", "smoke_cost_c3_greedy_traceon",
           "smoke_our_full_rerun", "smoke_our_full_block2")


def bundle_status(name: str) -> dict:
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
    rc = d / "run_config.json"
    if rc.is_file():
        cfg = json.loads(rc.read_text())
        status["config"] = cfg.get("config")
        status["git_hash"] = cfg.get("git_hash")
        status["git_dirty"] = cfg.get("git_dirty")
        status["binary_sha256"] = cfg.get("binary_sha256")
        status["schedule_seed"] = cfg.get("schedule_seed")
        if cfg.get("plugin_sha256"):
            status["plugin_sha256"] = cfg.get("plugin_sha256")
    res = d / "numa_residency.json"
    if res.is_file():
        data = json.loads(res.read_text())
        status["numa_off_target_pct"] = data.get("off_target_pct")
        status["numa_verdict"] = data.get("verdict")
    # BASELINE contract for c3 bundles
    log = d / "run.log"
    if log.is_file() and "cost_c3_greedy" in name:
        m = re.search(r"BASELINE cost_c3_greedy version=([0-9a-f]{40}) "
                      r"calibration_sha256=([0-9a-f]{64}) counter_set=([\w,]+)", log.read_text())
        if m:
            status["baseline_version_ok"] = m.group(1) == EXPECTED_GIT
            status["baseline_calibration_sha_ok"] = m.group(2) == EXPECTED_BASELINE_SHA
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
                    and status.get("git_hash") == EXPECTED_GIT
                    and status.get("git_dirty") in (False, None)
                    and status.get("baseline_version_ok", True)
                    and status.get("baseline_calibration_sha_ok", True)
                    and status.get("numa_verdict", "PASS") == "PASS")
    return status


def main() -> int:
    # Provenance: the recorded git_dirty=True in every bundle comes from the
    # UNTRACKED experiment/result output directories, not from modified tracked
    # sources. Verify that right now — if any tracked file were modified, the
    # bundles' binaries could not be attributed to HEAD and the smoke must fail.
    porcelain = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"],
                                        text=True).splitlines()
    untracked_only = not any(line and not line.startswith("??") for line in porcelain)
    bundles = [bundle_status(n) for n in BUNDLES]
    for b in bundles:
        if b.get("git_dirty") is True and untracked_only:
            b["git_dirty_attributable_to_untracked_outputs_only"] = True
            b["ok"] = (b.get("exit_code") == 0
                       and b.get("validator_valid") is True
                       and b.get("git_hash") == EXPECTED_GIT
                       and b.get("baseline_version_ok", True)
                       and b.get("baseline_calibration_sha_ok", True)
                       and b.get("numa_verdict", "PASS") == "PASS")
    proofs = json.loads((PHASE_B / "PROOFS.json").read_text())
    all_bundles_ok = all(b["ok"] for b in bundles)
    verdict = {
        "experiment": "EDBT_V4_PHASE_B",
        "stage": "implementation+build+smoke (NOT the formal E12/E13 runs)",
        "git_head": EXPECTED_GIT,
        "git_tracked_dirty_at_finalize": not untracked_only,
        "git_dirty_note": "bundle run_config git_dirty=true is caused solely by the "
                          "untracked experiments/edbt_v4/ and results_edbt_v4/ output "
                          "directories; zero tracked files are modified (verified at "
                          "finalize time)" if untracked_only else
                          "TRACKED FILES MODIFIED — provenance broken",
        "binary_sha256": bundles[0].get("binary_sha256") if bundles else None,
        "dram_budget_bytes": 3725884160,
        "off_target_page_threshold_pct": 5.0,
        "calibration_sha256": EXPECTED_BASELINE_SHA,
        "bundles": bundles,
        "all_bundles_ok": all_bundles_ok,
        "proofs_all_pass": proofs.get("ALL_PROOFS_PASS"),
        "proofs": {k: v.get("pass") for k, v in proofs.items() if isinstance(v, dict)},
        "notes": [
            "smoke_our_full_attempt1_harness_crash retained (benchmark exit 0, harness "
            "SameFileError in sidecar copy; fixed by skipping src==dst)",
            "preflight_c3 retained (12 s loader sanity for the aggregated calibration.csv)",
            "trace-off equivalence to the pre-B1 binary: smoke_runs/commit_b1_equivalence/",
        ],
    }
    verdict["SMOKE_PASS"] = bool(all_bundles_ok and proofs.get("ALL_PROOFS_PASS"))
    (PHASE_B / VERDICT).write_text(json.dumps(verdict, indent=2, ensure_ascii=False) + "\n")

    # File manifest over the whole phase-B tree, excluding the manifest itself.
    files = sorted(p for p in PHASE_B.rglob("*") if p.is_file() and p.name != MANIFEST)
    lines = []
    for p in files:
        h = hashlib.sha256()
        with p.open("rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        lines.append(f"{h.hexdigest()}  {p.relative_to(PHASE_B)}")
    (PHASE_B / MANIFEST).write_text("\n".join(lines) + "\n")
    print(f"{VERDICT}: SMOKE_PASS={verdict['SMOKE_PASS']}")
    print(f"{MANIFEST}: {len(lines)} files")
    for b in bundles:
        print(f"  {b['bundle']}: ok={b.get('ok')} exit={b.get('exit_code')} "
              f"validator={b.get('validator_valid')}")
    return 0 if verdict["SMOKE_PASS"] else 1


if __name__ == "__main__":
    sys.exit(main())
