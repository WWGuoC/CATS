"""Editorial-only vector replacements for Figures 2 and 4; no experiments.

Figure 4 provenance:
  Archive: vldb_experiments_summary.zip (original V1 summaries).
  Members: vldb_experiments_summary/cxlbench_{latency,bandwidth}_summary.csv.
  Original selectors: generate_figures_col.py, Figure 4, lines 236--285.
  Extracted inputs are exact archive-member bytes, including all source_file
  columns and unused rows. Existing CSVs are checked against their SHA-256
  digests first; the verified archive is needed only to extract missing inputs.
  Latencies use DDR/CXL random_latency (64 B) and sequential_latency (8 B).
  Bandwidth uses only CXL / seq_read / 8192 bytes at the original thread counts.
  The archived DDR bandwidth rows remain in the input but are not plotted.
  These are archived measurements, not a new hardware validation.

Figure 2 describes the recorded Phase C path using B5's all-storage enumeration
and LQP overlay (02_phase_b2/source_patches/0001-B5-*.patch), B2's deterministic
strict-fit mapper (01_phase_b/source_patches/0002-EDBT-v4-B2-*.patch), and the
configurable daemon period (default/TPC-H: 5 s; primary JOB: 1 s).
Cardinality is displayed as metadata;
the inverse-volume score uses encoded bytes, not a cardinality estimate.

Run with the same Matplotlib environment as generate_figures_phase_e.py.
By default, only Figure 4 is regenerated; the author-supplied Figure 2 is
preserved. Pass --redraw-figure2 explicitly to replace it with the editorial
diagram. Provenance is retained here and in each generated PDF's metadata.
"""
from __future__ import annotations

import argparse
import csv
import hashlib
import zipfile
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyArrowPatch, Rectangle


ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "figures"
ARCHIVE = ROOT / "vldb_experiments_summary.zip"
INPUT_DIR = ROOT / "v1_extracted" / "vldb_experiments_summary"
ARCHIVE_SHA256 = "fbb08d8473c80e76d6c07bc37d5f39cc27b95dd3c0b76ff595a8d62d8823493b"
INPUT_SHA256 = {
    "cxlbench_latency_summary.csv":
        "996e3f4a613fb39a2937620f1d8aa425cb51beb03d9717b63dc5b483151988b7",
    "cxlbench_bandwidth_summary.csv":
        "277987008d37b2c5cf6bcd7c56f4cd2d21c7a5e2d0455f5a0b71e8aa361c3e42",
}

BLUE = "#2E6FB7"
ORANGE = "#E8A33D"
GREEN = "#3A9D5D"
INK = "#28343B"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Arial", "DejaVu Sans"],
    "font.size": 8,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "axes.linewidth": 0.8,
})


def original_inputs():
    """Verify packaged CSVs first; extract missing inputs from the original archive."""
    missing = []
    for name, expected in INPUT_SHA256.items():
        destination = INPUT_DIR / name
        if not destination.exists():
            missing.append(name)
        elif hashlib.sha256(destination.read_bytes()).hexdigest() != expected:
            raise ValueError(f"Extracted input SHA-256 does not match: {destination}")

    if missing:
        if not ARCHIVE.is_file():
            raise FileNotFoundError(
                f"Missing CSV inputs: {', '.join(missing)}. "
                f"Provide the exact extracted CSVs in {INPUT_DIR} or the original {ARCHIVE}."
            )
        if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != ARCHIVE_SHA256:
            raise ValueError("Original V1 archive SHA-256 does not match")
        with zipfile.ZipFile(ARCHIVE) as archive:
            payloads = {}
            for name in missing:
                member = f"vldb_experiments_summary/{name}"
                payload = archive.read(member)
                if hashlib.sha256(payload).hexdigest() != INPUT_SHA256[name]:
                    raise ValueError(f"Archive member SHA-256 does not match: {member}")
                payloads[name] = payload
        INPUT_DIR.mkdir(parents=True, exist_ok=True)
        for name, payload in payloads.items():
            (INPUT_DIR / name).write_bytes(payload)

    tables = {}
    for name, expected in INPUT_SHA256.items():
        with (INPUT_DIR / name).open(newline="", encoding="utf-8-sig") as stream:
            tables[name] = list(csv.DictReader(stream))
        print(f"Input: vldb_experiments_summary/{name}; "
              f"rows={len(tables[name])}; sha256={expected}")
    return tables


def save(fig, name, subject):
    OUT.mkdir(parents=True, exist_ok=True)
    fig.savefig(OUT / f"{name}.pdf", metadata={
        "Title": name,
        "Subject": subject,
        "Creator": "generate_figures_editorial.py / Matplotlib",
    })
    plt.close(fig)


def figure2():
    # Keep the 3.33-inch column width and compress vertical spacing, not fonts.
    fig = plt.figure(figsize=(3.33, 4.80))
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set(xlim=(0, 3.33), ylim=(0, 5.15))
    ax.axis("off")

    def box(x, y, width, height, title, body, color, fill, size=7.2):
        ax.add_patch(Rectangle((x, y), width, height, facecolor=fill,
                               edgecolor=color, linewidth=0.8))
        ax.text(x + width / 2, y + height - 0.105, title, ha="center",
                va="top", fontsize=7.7, fontweight="bold", color=INK)
        ax.text(x + width / 2, y + (height - 0.24) / 2, body, ha="center",
                va="center", fontsize=size, color=INK, linespacing=1.25)

    def arrow(start, end):
        ax.add_patch(FancyArrowPatch(start, end, arrowstyle="-|>",
                                    mutation_scale=8, color=INK, linewidth=0.8,
                                    shrinkA=2, shrinkB=2))

    ax.text(0.10, 5.03, "BEFORE MEASUREMENT", fontsize=8, fontweight="bold",
            color=BLUE, va="top")
    box(0.10, 4.15, 1.49, 0.62, "Shared storage scan",
        "ALL sealed segments\n(all stored columns)", BLUE, "#F0F5FA")
    box(1.74, 4.15, 1.49, 0.62, "Workload LQP analysis",
        "Optimized logical plans\nResolve segment references", BLUE, "#F0F5FA", 7)
    box(0.10, 3.23, 3.13, 0.66, "Full-store segment profiles",
        "Overlay access-type proxies + cardinality metadata\n"
        "Unreferenced segments remain candidates (Unknown)", BLUE, "#F0F5FA", 7)
    arrow((0.845, 4.15), (0.845, 3.89))
    arrow((2.485, 4.15), (2.485, 3.89))

    ax.plot([0.10, 3.23], [3.06, 3.06], color="#B9C2C9", lw=0.6)
    ax.text(0.10, 2.94, "RUNTIME: DAEMON N s (default 5 s; JOB 1 s)", fontsize=7.5,
            fontweight="bold", color=GREEN, va="top")
    box(0.10, 2.19, 3.13, 0.53, "Counter update",
        "Typed access counters -> frequency EMA (alpha = 0.3)",
        GREEN, "#F2F7F3", 7)
    # Carry the initialized profiles across the phase boundary without implying
    # a query-time callback or an online refresh of LQP metadata.
    ax.plot([3.27, 3.27], [3.56, 2.455], color=INK, lw=0.8)
    ax.plot([3.23, 3.27], [3.56, 3.56], color=INK, lw=0.8)
    arrow((3.27, 2.455), (3.13, 2.455))

    box(0.10, 1.38, 3.13, 0.59, "Normalized heuristic score",
        "0.50 access-type proxy + 0.25 frequency\n"
        "+ 0.15 inverse encoded bytes + 0.10 immutable",
        GREEN, "#F2F7F3", 7.2)
    arrow((1.665, 2.19), (1.665, 1.97))
    box(0.10, 0.56, 3.13, 0.60, "Ranking + logical-budget packing",
        "Phase C: strict fit; skip segments that do not fit\n"
        "Equal-score ties: table, chunk, column",
        GREEN, "#F2F7F3", 7.1)
    arrow((1.665, 1.38), (1.665, 1.16))
    arrow((1.665, 0.56), (1.665, 0.32))
    ax.text(1.665, 0.20, "Migration requests: promote / demote", ha="center",
            va="center", fontsize=7.8, fontweight="bold", color=INK)
    save(fig, "fig2_engine",
         "Recorded Phase C flow: pre-measurement all-storage LQP overlay; "
         "configurable daemon period (default/TPC-H 5 s; primary JOB 1 s), "
         "normalized heuristic score, deterministic strict-fit "
         "logical segment-byte budget packing, and migration requests. "
         "Sources: B5 shared-enumerator patch and B2 strict-fit mapper patch.")


def figure4(tables):
    latencies = tables["cxlbench_latency_summary.csv"]
    bandwidth = tables["cxlbench_bandwidth_summary.csv"]
    latency_by_key = {(row["memory_type"], row["mode"]): row for row in latencies}
    assert len(latencies) == len(latency_by_key) == 4
    modes = [("random_latency", 64), ("sequential_latency", 8)]
    selected = sorted((row for row in bandwidth
                       if row["memory_type"] == "CXL"
                       and row["operation"] == "seq_read"
                       and int(row["access_size_bytes"]) == 8192),
                      key=lambda row: int(row["number_threads"]))
    threads = [int(row["number_threads"]) for row in selected]
    assert threads == [1, 4, 8, 16, 32, 64, 96]
    cxl_bw = [float(row["bandwidth_GiBs"]) for row in selected]

    fig, axes = plt.subplots(2, 1, figsize=(3.33, 4.15))
    fig.subplots_adjust(left=0.20, right=0.97, top=0.98, bottom=0.17, hspace=0.95)
    ax = axes[0]
    for memory, label, color, offset in [("DDR", "DRAM", BLUE, -0.17),
                                         ("CXL", "CXL", ORANGE, 0.17)]:
        vals = []
        for mode, access_size in modes:
            row = latency_by_key[(memory, mode)]
            assert int(row["access_size_bytes"]) == access_size
            vals.append(float(row["avg_ns"]))
        positions = [i + offset for i in range(2)]
        bars = ax.bar(positions, vals, width=0.34, color=color, label=label)
        ax.bar_label(bars, labels=[f"{value:.1f}" for value in vals],
                     padding=3, fontsize=7.5)
        print(f"Figure 4 latency ({label}, ns): {vals}")
    for i, (mode, _) in enumerate(modes):
        dram = float(latency_by_key[("DDR", mode)]["avg_ns"])
        cxl = float(latency_by_key[("CXL", mode)]["avg_ns"])
        ax.text(i, max(dram, cxl) + 140, f"{cxl / dram:.2f}x",
                ha="center", fontsize=8, color="#B03A48")
    ax.set_xticks([0, 1], ["Random\n64 B", "Sequential\n8 B"])
    ax.set_ylabel("Avg. access latency (ns)")
    ax.set_ylim(0, 950)
    ax.legend(frameon=False, loc="upper right", fontsize=7.5)
    ax.text(0.5, -0.40, "(a) Access latency", transform=ax.transAxes,
            ha="center", va="top", fontsize=8)

    ax = axes[1]
    ax.plot(threads, cxl_bw, "s-", color=ORANGE, label="CXL", ms=3.2)
    ax.annotate(f"{cxl_bw[-1]:.2f} GiB/s", (threads[-1], cxl_bw[-1]),
                textcoords="offset points", xytext=(-2, 8), ha="right",
                fontsize=7.5, color="#825B18")
    ax.set_xscale("log", base=2)
    ax.set_xticks(threads, threads)
    ax.minorticks_off()
    ax.set(xlim=(0.8, 130), ylim=(0, 32), xlabel="Threads",
           ylabel="Seq. read bandwidth (GiB/s)")
    ax.set_yticks([0, 10, 20, 30])
    ax.legend(frameon=False, loc="lower right", fontsize=7.5)
    ax.text(0.5, -0.43, "(b) CXL sequential read (8 KiB)", transform=ax.transAxes,
            ha="center", va="top", fontsize=8)
    for ax in axes:
        ax.spines[["top", "right"]].set_visible(False)
        ax.grid(axis="y", alpha=0.25, lw=0.5)
        ax.set_axisbelow(True)
    print(f"Figure 4 CXL bandwidth (threads, GiB/s): {list(zip(threads, cxl_bw))}")
    save(fig, "fig4_microbench",
         f"Original V1 archive SHA256={ARCHIVE_SHA256}; "
         + "; ".join(f"{name} SHA256={digest}" for name, digest in INPUT_SHA256.items())
         + "; selectors: DDR/CXL random_latency 64 B and sequential_latency 8 B; "
         "CXL seq_read 8192 B only. Original per-row source_file fields preserved. "
         "DDR bandwidth is intentionally not plotted; no new measurements.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--redraw-figure2", action="store_true",
                        help="Replace the current Figure 2 with the editorial diagram")
    args = parser.parse_args()
    inputs = original_inputs()
    if args.redraw_figure2:
        figure2()
    figure4(inputs)
    print("Generated Figure 4; " + ("redrew Figure 2." if args.redraw_figure2 else "preserved Figure 2."))
