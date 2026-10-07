#!/usr/bin/env python3
"""EDBT v4 Phase B2 smoke runner (B2-10).

Re-runs the required smoke matrix on the B5 commit and invokes the phase_b2
validator with --create-stamp (a stamp binds the validator's own SHA-256 and
the validation report's SHA-256; never written after a failed check).

B2 evidence bundles:
  trace-on x3    -> initial==daemon candidate-set identity (B2-2), all-policy
                    typed-counter tail columns (B2-4), dictionary share (B2-8)
  measured x1    -> single-client measured diagnostic pass (B2-5)
  trace-off x3   -> policy arms on the new binaries
  reference x2   -> dram_only / cxl_only
  rerun x1       -> same-seed determinism probe

Every run_config.json records runner_sha256 (this file) and validator_sha256
(B2-1). Output: results_edbt_v4/02_phase_b2/smoke_runs/.
"""

import argparse
import csv
import json
import math
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path("/home/dameng/CXL_migration/hyrise")
PACK = Path("/home/dameng/CXL_migration/edbt_phase_b2_extract/CXL 页面迁移/edbt_experiment_pack")
BIN = ROOT / "build/hyriseBenchmarkJoinOrder"
PLUGIN = ROOT / "build/lib/libhyriseSemanticPlacementPlugin.so"
VALIDATOR = PACK / "scripts/validate_run_bundle.py"
OUTROOT = ROOT / "results_edbt_v4/02_phase_b2/smoke_runs"
CALIB = ROOT / "results_edbt_v4/02_phase_b2/calibration/calibration.csv"

DRAM_BUDGET_BYTES = 3725884160
CXL_GB = 100.0
DEMOTE_PCT = 0.95
OFF_TARGET_PAGE_THRESHOLD_PCT = 5.0

COMMON = ["--table_path", "imdb_data", "--queries", "all", "--mode", "Shuffled",
          "--clients", "10", "--scheduler", "--cores", "0", "--dont_cache_binary_tables",
          "--warmup", "0"]

MIGRATION_BASE = ["--plugins", str(PLUGIN), "--semantic", "--dram_first",
                  "--rmem_ids", "2", "--lmem_ids", "1",
                  "--dram_capacity_gb", "3.47", "--cxl_capacity_gb", str(CXL_GB),
                  "--demote_pct", str(DEMOTE_PCT), "--daemon_period", "1"]


def sha256(path: Path) -> str:
    return subprocess.check_output(["sha256sum", str(path)], text=True).split()[0]


def git_hash() -> str:
    return subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()


def git_tracked_dirty() -> bool:
    st = subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"], text=True)
    return any(line and not line.startswith("??") for line in st.splitlines())


def sample_numa_maps(pid: int, out: Path) -> dict:
    try:
        text = Path(f"/proc/{pid}/numa_maps").read_text()
    except OSError as exc:
        return {"error": str(exc)}
    out.write_text(text, encoding="utf-8")
    per_node: dict[str, int] = {}
    total = 0
    for line in text.splitlines():
        for token in line.split():
            if token.startswith("N") and "=" in token:
                node, _, value = token.partition("=")
                try:
                    pages = int(value)
                except ValueError:
                    continue
                per_node[node] = per_node.get(node, 0) + pages
                total += pages
    off = {n: p for n, p in per_node.items() if n not in ("N0", "N1", "N2")}
    return {"per_node_pages": per_node, "total_pages": total, "other_nodes": off}


def summarize(benchmark_json: Path, run_dir: Path, config: str, rep: int) -> None:
    data = json.loads(benchmark_json.read_text())
    per_item: dict[str, list[float]] = {}
    for b in data.get("benchmarks", []):
        runs = [r["duration"] / 1e6 for r in b.get("successful_runs", [])]
        per_item[b["name"]] = runs
    items = sorted(per_item)
    means = []
    with (run_dir / "per_query.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["item_name", "n_runs", "avg_ms"])
        for name in items:
            runs = per_item[name]
            avg = sum(runs) / len(runs) if runs else float("nan")
            means.append(avg)
            w.writerow([name, len(runs), f"{avg:.6f}"])
    geomean = math.exp(sum(math.log(m) for m in means) / len(means)) if means and all(m > 0 for m in means) else float("nan")
    with (run_dir / "summary.csv").open("w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["config", "rep", "n_items", "geomean_avg_ms"])
        w.writerow([config, rep, len(items), f"{geomean:.6f}"])


def run_bundle(name: str, config: str, rep: int, extra: list[str], membind: str | None,
               trace: bool, time_s: int, schedule_seed: int | None,
               measured: bool = False, clients: int = 10) -> Path:
    run_dir = OUTROOT / name
    if run_dir.exists():
        shutil.rmtree(run_dir)
    run_dir.mkdir(parents=True)
    bjson = run_dir / "benchmark.json"

    common = list(COMMON)
    common[common.index("--clients") + 1] = str(clients)
    cmd = ["numactl", "-N", "1"]
    if membind:
        cmd += ["-m", membind]
    cmd += [str(BIN)] + common + ["--time", str(time_s)]
    if schedule_seed is not None:
        cmd += ["--schedule_seed", str(schedule_seed)]
    cmd += extra
    if trace:
        cmd += ["--placement_trace"]
    if measured:
        cmd += ["--placement_trace_measured"]
    cmd += ["-o", str(bjson)]

    env = dict(os.environ)
    if trace:
        env["EDBT_V4_RUN_ID"] = name
        env["EDBT_V4_REP"] = str(rep)

    cfg: dict = {
        "config": config, "rep": rep, "experiment": "PHASE_B2_SMOKE",
        "git_hash": git_hash(), "git_tracked_dirty": git_tracked_dirty(),
        "runner": str(Path(__file__).resolve()),
        "runner_sha256": sha256(Path(__file__).resolve()),
        "validator": str(VALIDATOR), "validator_sha256": sha256(VALIDATOR),
        "binary_sha256": sha256(BIN), "command": " ".join(cmd),
        "trace": trace, "trace_measured": measured, "time_s": time_s, "clients": clients,
        "cpu_binding": "numactl -N 1", "dram_budget_bytes": None,
        "off_target_page_threshold_pct": OFF_TARGET_PAGE_THRESHOLD_PCT,
        "start_ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    if config != "dram_only" and config != "cxl_only":
        cfg["plugin_sha256"] = sha256(PLUGIN)
        cfg["dram_budget_bytes"] = DRAM_BUDGET_BYTES
        cfg["dram_capacity_gb_cli"] = 3.47
        cfg["budget_note"] = ("float32(3.47)*2^30 truncated; code multiplies by SIZE_GIB "
                              "(GiB semantics despite the _gb flag name)")
        cfg["membind"] = "lmem=1 (plugin LinearNumaMemoryResource), rmem=2"
    else:
        cfg["membind"] = f"numactl -N 1 -m {membind} (strict MPOL_BIND, no fallback)"
    if config == "cost_c3_greedy":
        cfg["c3_calibration"] = str(CALIB)
        cfg["c3_calibration_sha256"] = sha256(CALIB)
    if schedule_seed is not None:
        cfg["schedule_seed"] = schedule_seed

    print(f"[RUN ] {name} seed={schedule_seed} trace={trace} measured={measured} clients={clients}", flush=True)
    logf = (run_dir / "run.log").open("w")
    proc = subprocess.Popen(cmd, stdout=logf, stderr=subprocess.STDOUT, cwd=str(ROOT), env=env)
    exit_code = None
    numa_residency = None
    if membind:
        time.sleep(max(20, min(60, time_s // 2)))
        for _ in range(20):
            if proc.poll() is not None:
                break
            if Path(f"/proc/{proc.pid}/numa_maps").exists():
                numa_residency = sample_numa_maps(proc.pid, run_dir / "numa_maps_sample.txt")
                break
            time.sleep(5)
        else:
            numa_residency = sample_numa_maps(proc.pid, run_dir / "numa_maps_sample.txt")
    exit_code = proc.wait()
    logf.close()
    (run_dir / "exit_code.txt").write_text(f"{exit_code}\n")
    print(f"[DONE] {name} exit={exit_code}", flush=True)

    if numa_residency is not None and "per_node_pages" in numa_residency:
        target = "N1" if membind == "1" else "N2"
        total = numa_residency["total_pages"] or 1
        off_pct = 100.0 * (total - numa_residency["per_node_pages"].get(target, 0)) / total
        verdict = "PASS" if off_pct <= OFF_TARGET_PAGE_THRESHOLD_PCT else "FAIL"
        print(f"[NUMA] {name}: off-target {off_pct:.3f}% -> {verdict}", flush=True)
        numa_residency.update(target_node=target, off_target_pct=off_pct,
                              threshold_pct=OFF_TARGET_PAGE_THRESHOLD_PCT, verdict=verdict)
    if numa_residency is not None:
        (run_dir / "numa_residency.json").write_text(json.dumps(numa_residency, indent=2) + "\n")

    if exit_code == 0 and bjson.exists():
        summarize(bjson, run_dir, config, rep)
        stem = str(bjson)[:-len(".json")]
        for suffix, dest in (("_semantic_placement.csv", "benchmark_semantic_placement.csv"),
                             ("_daemon_metrics.csv", "daemon_metrics.csv"),
                             ("_decision_trace.csv", "decision_trace.csv"),
                             ("_operator_segment_trace.csv", "operator_segment_trace.csv"),
                             ("_measured_segment_access.csv", "measured_segment_access.csv"),
                             ("_schedule_plan.csv", "schedule_plan.csv"),
                             ("_schedule_manifest.csv", "schedule_manifest.csv")):
            src = Path(stem + suffix)
            if src.exists() and src != run_dir / dest:
                shutil.copy2(src, run_dir / dest)
        if (run_dir / "schedule_plan.csv").exists():
            cfg["query_schedule_sha256"] = sha256(run_dir / "schedule_plan.csv")

    (run_dir / "run_config.json").write_text(json.dumps(cfg, indent=2) + "\n")

    # Validator with stamp creation (B2-10): a stamp is written only when all
    # checks pass and binds the validator SHA-256 + validation-report SHA-256.
    vcmd = [sys.executable, str(VALIDATOR), str(run_dir), "--create-stamp"]
    if trace:
        vcmd += ["--require-trace", "--budget-bytes", str(DRAM_BUDGET_BYTES)]
    elif config not in ("dram_only", "cxl_only"):
        vcmd += ["--budget-bytes", str(DRAM_BUDGET_BYTES)]
    vres = subprocess.run(vcmd, capture_output=True, text=True)
    (run_dir / "validator_stdout.json").write_text(vres.stdout)
    ok = vres.returncode == 0
    stamped = (run_dir / "validated.ok").exists()
    print(f"[VALIDATE] {name}: {'OK' if ok else 'FAIL'} stamped={stamped}", flush=True)
    return run_dir


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--time", type=int, default=120)
    ap.add_argument("--only", type=str, default="")
    args = ap.parse_args()
    OUTROOT.mkdir(parents=True, exist_ok=True)

    seed = 101  # block 1 smoke schedule (same as Phase B for comparability)
    plans = [
        ("smoke_b2_our_full", "our_full", 1, MIGRATION_BASE, None, False, seed, dict()),
        ("smoke_b2_our_freq", "our_freq", 1, MIGRATION_BASE + ["--ablate_d1"], None, False, seed, dict()),
        ("smoke_b2_cost_c3_greedy", "cost_c3_greedy", 1,
         MIGRATION_BASE + ["--cost_c3_greedy", "--c3_calibration", str(CALIB)], None, False, seed, dict()),
        ("smoke_b2_dram_only", "dram_only", 1, [], "1", False, seed, dict()),
        ("smoke_b2_cxl_only", "cxl_only", 1, [], "2", False, seed, dict()),
        ("smoke_b2_our_full_traceon", "our_full", 1, MIGRATION_BASE, None, True, seed, dict()),
        ("smoke_b2_our_freq_traceon", "our_freq", 1, MIGRATION_BASE + ["--ablate_d1"], None, True, seed, dict()),
        ("smoke_b2_cost_c3_greedy_traceon", "cost_c3_greedy", 1,
         MIGRATION_BASE + ["--cost_c3_greedy", "--c3_calibration", str(CALIB)], None, True, seed, dict()),
        # B2-5 single-client measured diagnostic (trace + measured, clients=1)
        ("smoke_b2_our_full_measured", "our_full", 1, MIGRATION_BASE, None, True, seed,
         dict(measured=True, clients=1, time_s=60)),
        # Determinism probes (strict-fit mapper + schedule replay)
        ("smoke_b2_our_full_rerun", "our_full", 1, MIGRATION_BASE, None, False, seed, dict()),
    ]
    if args.only:
        keep = set(args.only.split(","))
        plans = [p for p in plans if p[0] in keep]
    failures = []
    for name, config, rep, extra, membind, trace, s, kw in plans:
        time_s = kw.pop("time_s", args.time)
        try:
            run_bundle(name, config, rep, extra, membind, trace, time_s, s, **kw)
        except Exception as exc:  # keep the matrix running; record the failure
            import traceback
            failures.append((name, repr(exc)))
            print(f"[BUNDLE FAILED] {name}: {exc!r}", flush=True)
            traceback.print_exc()
    print(f"[ALL B2 SMOKE DONE] failures={failures if failures else 'none'}")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
