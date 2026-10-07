#!/bin/bash
# EDBT v4 Phase B — commit B1 trace-off equivalence proof runner.
# Proves (before any mapper change, commit B2):
#   A) base(0f182ad07) vs B1, daemon OFF  -> placement CSV byte-identical,
#      filtered run.log identical (deterministic decision path)
#   B) base vs base, daemon ON (control)  -> placement CSV identical, but
#      [Daemon] cycle lines differ run-to-run (pre-existing nondeterminism
#      from Shuffled access interleaving, NOT caused by B1)
#   C) base vs B1, daemon ON              -> placement CSV byte-identical
#   D) B1 with --placement_trace          -> emitter works; rows honestly
#      record the v3 mapper boundary overshoot (selected=true,
#      fits_remaining=false) at this commit
# All runs: numactl -N 1, JOB 113, Shuffled, clients 10, c05 budget
# (3.47 GiB DRAM logical / 100 GiB CXL), demote_pct 0.95, dram_first, 45 s.
set -uo pipefail
ROOT=/home/dameng/CXL_migration/hyrise
OUT="$ROOT/results_edbt_v4/01_phase_b/smoke_runs/commit_b1_equivalence"
B1BIN="$ROOT/build/hyriseBenchmarkJoinOrder"
B1PLUG="$ROOT/build/lib/libhyriseSemanticPlacementPlugin.so"
# Clean full rebuild of the base commit 0f182ad07 in a separate worktree
# (incremental revert-rebuild in the main tree produced a corrupt binary via
# stale objects after the types.hpp layout change; that attempt is kept as
# build_logs/build_base.log + a failure note, the binaries were discarded).
BASEBIN="/home/dameng/CXL_migration/hyrise_base_wt/build_base/hyriseBenchmarkJoinOrder"
BASEPLUG="/home/dameng/CXL_migration/hyrise_base_wt/build_base/lib/libhyriseSemanticPlacementPlugin.so"
TIME_S=45
mkdir -p "$OUT"

run_one() { # run_one <name> <bin> <plugin> <extra args...>
  local name=$1; shift
  local bin=$1; shift
  local plug=$1; shift
  local d="$OUT/$name"
  rm -rf "$d"; mkdir -p "$d"
  local cmd=(numactl -N 1 "$bin" --table_path imdb_data --queries all
             --mode Shuffled --time $TIME_S --clients 10 --scheduler --cores 0
             --dont_cache_binary_tables --warmup 0
             --plugins "$plug" --semantic --dram_first
             --rmem_ids 2 --lmem_ids 1
             --dram_capacity_gb 3.47 --cxl_capacity_gb 100.0 --demote_pct 0.95
             -o "$d/benchmark.json" "$@")
  echo "[RUN ] $name $(date -Is)" | tee -a "$OUT/EQ_PROGRESS.log"
  echo "  cmd: ${cmd[*]}" >> "$OUT/EQ_PROGRESS.log"
  ( cd "$ROOT" && "${cmd[@]}" > "$d/run.log" 2>&1 )
  echo $? > "$d/exit_code.txt"
  sha256sum "$bin" "$plug" > "$d/binary_hashes.txt"
  echo "[DONE] $name exit=$(cat "$d/exit_code.txt") $(date -Is)" | tee -a "$OUT/EQ_PROGRESS.log"
}

# --- Pair A: deterministic decision path (daemon off) ---
run_one eq_base_daemon0 "$BASEBIN" "$BASEPLUG" --daemon_period 0
run_one eq_b1_daemon0   "$B1BIN"   "$B1PLUG"   --daemon_period 0
# --- Control pair B + pair C: daemon on ---
run_one eq_base_d1_run1 "$BASEBIN" "$BASEPLUG" --daemon_period 1
run_one eq_base_d1_run2 "$BASEBIN" "$BASEPLUG" --daemon_period 1
run_one eq_b1_d1        "$B1BIN"   "$B1PLUG"   --daemon_period 1
# --- D: trace emitter smoke (B1, trace on) ---
EDBT_V4_RUN_ID=trc_b1_d1 EDBT_V4_REP=1 run_one trc_b1_d1 "$B1BIN" "$B1PLUG" \
    --daemon_period 1 --placement_trace
echo "[ALL DONE] $(date -Is)" | tee -a "$OUT/EQ_PROGRESS.log"
