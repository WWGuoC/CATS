#!/usr/bin/env python3
"""EDBT v4 Phase B2 (B2-6): aggregate the calibration bench v2 raw CSV.

All repeats are retained (no outcome-dependent deletion). Per
(device, gamma, data_type) cell over the `ns_per_access_per_byte` values:
  median, MAD, IQR, min/max, and a distribution-free 95% CI for the median
  computed from order statistics (exact binomial coverage, no bootstrap).

Outputs (in --outdir):
  calibration.csv          loader-compatible median table
                           (device,gamma,data_type,bytes_per_value,ns_per_access_per_byte)
  calibration_stats.csv    per-cell dispersion/uncertainty (feeds B2-7 endpoints)
  calibration_report.txt   physical-sanity ratios (CXL/DRAM per cell) + checks

Usage: aggregate_calibration_v2.py <calibration_raw_v2.csv> <outdir> [--llc-bytes N]
"""

import argparse
import csv
import math
from collections import defaultdict
from pathlib import Path

GAMMAS = ["sequential", "monotonic", "random", "point"]
TYPES = ["int32", "int64", "float32", "float64", "string16"]


def binomial_ci_bounds(n: int, alpha: float = 0.05) -> tuple[int, int]:
    """Order-statistic indices (1-based k, n+1-k) with exact coverage >= 1-alpha
    for the median: P(X_(k) <= m <= X_(n+1-k)) = 1 - 2*P(Bin(n,0.5) <= k-1)."""
    def binom_cdf_le(k: int) -> float:  # P(Bin(n, 0.5) <= k)
        return sum(math.comb(n, i) * 0.5 ** n for i in range(k + 1))
    k = 1
    while k <= n // 2 and binom_cdf_le(k - 1) <= alpha / 2:
        k += 1
    k -= 1  # largest k whose coverage is still >= 1 - alpha
    return max(k, 1), n + 1 - max(k, 1)


def median(xs: list[float]) -> float:
    s = sorted(xs)
    n = len(s)
    return s[n // 2] if n % 2 else 0.5 * (s[n // 2 - 1] + s[n // 2])


def percentile_simple(xs: list[float], q: float) -> float:
    s = sorted(xs)
    idx = q * (len(s) - 1)
    lo, hi = int(math.floor(idx)), int(math.ceil(idx))
    return s[lo] if lo == hi else s[lo] + (idx - lo) * (s[hi] - s[lo])


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw_csv")
    ap.add_argument("outdir")
    ap.add_argument("--llc-bytes", type=int, default=452984832,
                    help="per-socket LLC bytes (default 432 MiB); every cell's "
                         "working_set_bytes must exceed this")
    ap.add_argument("--min-repeats", type=int, default=10)
    args = ap.parse_args()

    raw_path = Path(args.raw_csv)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    rows = list(csv.DictReader(open(raw_path)))
    assert rows, "raw csv is empty"

    cells = defaultdict(list)
    problems: list[str] = []
    for r in rows:
        cell = (r["device"], r["gamma"], r["data_type"])
        cells[cell].append(r)
        ws = int(r["working_set_bytes"])
        if ws <= args.llc_bytes:
            problems.append(f"working set {ws} <= LLC {args.llc_bytes} in {cell} repeat {r['repeat']}")

    # Grid completeness + repeat count
    expected = {(d, g, t) for d in ("dram", "cxl") for g in GAMMAS for t in TYPES}
    missing = expected - set(cells)
    if missing:
        problems.append(f"missing cells: {sorted(missing)}")
    for cell, rs in cells.items():
        repeats = sorted(int(r["repeat"]) for r in rs)
        if len(rs) < args.min_repeats:
            problems.append(f"{cell}: only {len(rs)} repeats (< {args.min_repeats})")
        if repeats != list(range(len(repeats))):
            problems.append(f"{cell}: non-contiguous repeat ids {repeats}")
        bpvs = {r["bytes_per_value"] for r in rs}
        if len(bpvs) != 1:
            problems.append(f"{cell}: inconsistent bytes_per_value {bpvs}")

    # Cell order randomization check (fixed seed shuffle => non-systematic order)
    order_by_cell = {(r["device"], r["gamma"], r["data_type"]): int(r["cell_order"]) for r in rows}
    if sorted(order_by_cell.values()) != list(range(40)):
        problems.append("cell_order is not a permutation of 0..39")

    stats = {}
    for cell, rs in sorted(cells.items()):
        device, gamma, dtype = cell
        xs = [float(r["ns_per_access_per_byte"]) for r in rs]
        n = len(xs)
        s = sorted(xs)
        med = median(xs)
        mad = median([abs(x - med) for x in xs])
        iqr = percentile_simple(xs, 0.75) - percentile_simple(xs, 0.25)
        k, k_hi = binomial_ci_bounds(n)
        ci_low, ci_high = s[k - 1], s[k_hi - 1]
        stats[cell] = dict(n=n, median=med, mad=mad, iqr=iqr, min=s[0], max=s[-1],
                           ci_low=ci_low, ci_high=ci_high,
                           rel_half_width=max(med - ci_low, ci_high - med) / med if med else 0.0,
                           bpv=int(rs[0]["bytes_per_value"]),
                           order=order_by_cell[cell])

    # loader-compatible median table (grid order for readability)
    with open(outdir / "calibration.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["device", "gamma", "data_type", "bytes_per_value", "ns_per_access_per_byte"])
        for device in ("dram", "cxl"):
            for gamma in GAMMAS:
                for dtype in TYPES:
                    st = stats.get((device, gamma, dtype))
                    if st:
                        w.writerow([device, gamma, dtype, st["bpv"], f"{st['median']:.9f}"])

    with open(outdir / "calibration_stats.csv", "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["device", "gamma", "data_type", "cell_order", "n", "bytes_per_value",
                    "median_ns_per_access_per_byte", "mad", "iqr", "min", "max",
                    "ci95_low", "ci95_high", "rel_half_width"])
        for cell in sorted(stats, key=lambda c: stats[c]["order"]):
            st = stats[cell]
            w.writerow([cell[0], cell[1], cell[2], st["order"], st["n"], st["bpv"],
                        f"{st['median']:.9f}", f"{st['mad']:.9f}", f"{st['iqr']:.9f}",
                        f"{st['min']:.9f}", f"{st['max']:.9f}",
                        f"{st['ci_low']:.9f}", f"{st['ci_high']:.9f}", f"{st['rel_half_width']:.6f}"])

    # physical sanity: CXL/DRAM median ratio per cell
    lines = [f"# aggregate_calibration_v2 over {raw_path}", f"# rows={len(rows)} cells={len(cells)}"]
    if problems:
        lines.append("# PROBLEMS:")
        lines.extend(f"#   {p}" for p in problems)
    else:
        lines.append("# checks: grid complete, all cells >= min repeats, "
                     "working sets > LLC, cell_order is a permutation, single bpv per cell")
    lines.append("")
    lines.append("cell (gamma, dtype): dram med | cxl med | cxl/dram | CI half-width rel")
    for gamma in GAMMAS:
        for dtype in TYPES:
            d = stats.get(("dram", gamma, dtype))
            c = stats.get(("cxl", gamma, dtype))
            if not (d and c):
                continue
            ratio = c["median"] / d["median"] if d["median"] else float("nan")
            lines.append(f"  {gamma:10s} {dtype:8s}: {d['median']:.6f} | {c['median']:.6f} | "
                         f"{ratio:6.2f}x | {max(d['rel_half_width'], c['rel_half_width'])*100:.1f}%")
    report = "\n".join(lines) + "\n"
    open(outdir / "calibration_report.txt", "w").write(report)
    print(report)
    raise SystemExit(1 if problems else 0)


if __name__ == "__main__":
    main()
