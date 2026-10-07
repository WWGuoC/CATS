#!/usr/bin/env python3
"""EDBT v4 Phase B: aggregate c3_calibration_bench raw output into the loader CSV.

Input  (calibration_raw.csv): device,gamma,data_type,repeat,accesses,buffer_bytes,
                               bytes_per_value,ns_total,ns_per_access,ns_per_access_per_byte
                               (2 devices x 5 data types x 4 gammas x kRepeats rows)
Output (calibration.csv):      device,gamma,data_type,bytes_per_value,ns_per_access_per_byte
                               (40 rows = 2 x 5 x 4; median over repeats)

The plugin loader (load_c3_calibration) requires exactly this header and a
complete lowercase dram/cxl x {sequential,monotonic,random,point} x
{int32,int64,float32,float64,string16} grid with positive finite rates.

Also writes <out>.stats.csv (median, mean, stdev, cv%) for the method report.
"""
import argparse
import collections
import csv
import statistics

GAMMAS = ("sequential", "monotonic", "random", "point")
DTYPES = ("int32", "int64", "float32", "float64", "string16")
DEVICES = ("dram", "cxl")
BYTES_PER_VALUE = {"int32": 4, "int64": 8, "float32": 4, "float64": 8, "string16": 16}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("raw_csv")
    ap.add_argument("out_csv")
    args = ap.parse_args()

    samples = collections.defaultdict(list)
    bpv_seen = {}
    with open(args.raw_csv, newline="") as f:
        reader = csv.DictReader(f)
        expected = ["device", "gamma", "data_type", "repeat", "accesses", "buffer_bytes",
                    "bytes_per_value", "ns_total", "ns_per_access", "ns_per_access_per_byte"]
        if reader.fieldnames != expected:
            raise SystemExit(f"unexpected raw header: {reader.fieldnames}")
        for row in reader:
            key = (row["device"], row["gamma"], row["data_type"])
            samples[key].append(float(row["ns_per_access"]))
            bpv_seen[key] = int(row["bytes_per_value"])

    # Grid completeness + dtype sanity (buffer byte width must match the declared dtype).
    missing = [k for k in ((d, g, t) for d in DEVICES for g in GAMMAS for t in DTYPES)
               if k not in samples]
    if missing:
        raise SystemExit(f"missing grid cells: {missing}")
    for key, bpv in bpv_seen.items():
        if bpv != BYTES_PER_VALUE[key[2]]:
            raise SystemExit(f"bytes_per_value mismatch at {key}: {bpv}")

    with open(args.out_csv, "w", newline="") as f:
        f.write("device,gamma,data_type,bytes_per_value,ns_per_access_per_byte\n")
        # Deterministic row order: device, gamma (as measured), dtype.
        for device in DEVICES:
            for gamma in GAMMAS:
                for dtype in DTYPES:
                    key = (device, gamma, dtype)
                    v = samples[key]
                    if len(v) < 3:
                        raise SystemExit(f"too few repeats at {key}: {len(v)}")
                    median_ns = statistics.median(v)
                    rate = median_ns / BYTES_PER_VALUE[dtype]
                    f.write(f"{device},{gamma},{dtype},{BYTES_PER_VALUE[dtype]},{rate:.9f}\n")

    with open(args.out_csv + ".stats.csv", "w", newline="") as f:
        f.write("device,gamma,data_type,n_repeats,ns_per_access_median,ns_per_access_mean,"
                "ns_per_access_stdev,cv_pct\n")
        for device in DEVICES:
            for gamma in GAMMAS:
                for dtype in DTYPES:
                    key = (device, gamma, dtype)
                    v = samples[key]
                    mean = statistics.mean(v)
                    f.write(f"{device},{gamma},{dtype},{len(v)},{statistics.median(v):.6f},"
                            f"{mean:.6f},{statistics.stdev(v):.6f},"
                            f"{statistics.stdev(v) / mean * 100:.2f}\n")
    print(f"wrote {args.out_csv} ({len(bpv_seen)} grid cells) and {args.out_csv}.stats.csv")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
