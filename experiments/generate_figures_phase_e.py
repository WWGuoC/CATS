"""Generate Phase-E paper figures 5--9 from validated Phase C/D and repaired Weisgut summaries.

Inputs are read-only validated analysis tables.  The script writes figures 5--9
to the repository's ``figures`` directory. Figures 2 and 4 are maintained by
``generate_figures_editorial.py``.
"""
from __future__ import annotations

import csv
import json
import math
import statistics as st
from pathlib import Path

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt


ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / "figures"
OUT.mkdir(parents=True, exist_ok=True)
C_METRICS = ROOT / "03_phase_c" / "analysis" / "core" / "validated_run_metrics.csv"
C_STATS = ROOT / "03_phase_c" / "analysis" / "core" / "core_statistics.json"
D_LEDGER = ROOT / "05_phase_d" / "analysis" / "ledger.csv"
D_STATS = ROOT / "05_phase_d" / "analysis" / "family_statistics.json"
W_RUNS = ROOT / "08_weisgut_repair_rerun" / "analysis" / "run_metrics.csv"

BLUE = "#2E6FB7"
LIGHT_BLUE = "#8AB6E0"
ORANGE = "#E69F00"
GREEN = "#3A9D5D"
PURPLE = "#8C6BB1"
GRAY = "#7F7F7F"
LIGHT_GRAY = "#C9C9C9"
RED = "#B03A48"

plt.rcParams.update({
    "font.family": "sans-serif",
    "font.sans-serif": ["Arial", "DejaVu Sans"],
    "font.size": 8,
    "pdf.fonttype": 42,
    "ps.fonttype": 42,
    "axes.linewidth": 0.8,
})


def read_csv(path: Path):
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def style(ax, zero=False):
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="y", alpha=0.25, lw=0.5)
    ax.set_axisbelow(True)
    if zero:
        ax.axhline(0, color="black", lw=0.7)


def style_hbar(ax):
    ax.spines[["top", "right"]].set_visible(False)
    ax.grid(axis="x", alpha=0.25, lw=0.5)
    ax.set_axisbelow(True)
    ax.axvline(0, color="black", lw=0.7)


def save(fig, name):
    fig.tight_layout(pad=0.5)
    fig.savefig(OUT / f"{name}.pdf", bbox_inches="tight", pad_inches=0.03)
    plt.close(fig)


def values(rows, **want):
    out = []
    for row in rows:
        if all(str(row.get(k, "")) == str(v) for k, v in want.items()):
            out.append(float(row["geomean_ms"]))
    return out


def gm(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs))


def bar_with_range(ax, x, xs, color, label=None, width=0.22, mid_fn=st.median, hatch=None):
    mid = mid_fn(xs)
    err = [[mid - min(xs)], [max(xs) - mid]]
    ax.bar(x, mid, width, color=color, label=label, yerr=err, capsize=2,
           hatch=hatch, edgecolor="black", linewidth=0.35, error_kw={"lw": 0.8})
    return mid


c_rows = read_csv(C_METRICS)
d_rows = read_csv(D_LEDGER)
w_rows = read_csv(W_RUNS)
with C_STATS.open(encoding="utf-8") as f:
    c_stats = json.load(f)["comparisons"]
with D_STATS.open(encoding="utf-8") as f:
    d_stats = json.load(f)["comparisons"]


# Figure 5: four-way overall comparison within the repaired campaign.
fig, axes = plt.subplots(1, 2, figsize=(7.0, 2.7),
                         gridspec_kw={"width_ratios": [1.1, 1]})
ax = axes[0]
budgets = ["c05", "c10", "c20"]
width = 0.28
def w_values(**want):
    return [float(row["geomean_avg_ms"]) for row in w_rows
            if all(str(row.get(k, "")) == str(v) for k, v in want.items())]

for j, (cfg, label, color) in enumerate([
        ("our_full", "Our-full", BLUE),
        ("weisgut_static_column", "Weisgut-static", ORANGE)]):
    for i, b in enumerate(budgets):
        xs = w_values(family="W2_TPCH_BUDGET", config=cfg, budget_label=b)
        assert len(xs) == 3, (cfg, b)
        bar_with_range(ax, i + (j - 0.5) * width, xs, color,
                       label=label if i == 0 else None, width=width,
                       mid_fn=st.mean)
for cfg, label, color, linestyle in [
        ("dram_only", "DRAM-only", GREEN, "--"),
        ("cxl_only", "CXL-only", GRAY, ":")]:
    xs = w_values(family="W2_TPCH_BUDGET", config=cfg)
    assert len(xs) == 3, cfg
    ax.axhspan(min(xs), max(xs), color=color, alpha=0.12, linewidth=0)
    ax.axhline(st.mean(xs), color=color, linestyle=linestyle, lw=1.2, label=label)
ax.set_xticks(range(3), ["4.1 GiB", "8.2 GiB", "16.4 GiB"])
ax.set_xlabel("Logical DRAM budget")
ax.set_ylabel("Geomean latency (ms)")
ax.set_ylim(0, 10500)
ax.set_yticks([0, 2500, 5000, 7500, 10000])
ax.set_title("(a) TPC-H SF100", loc="left", fontsize=9)
handles, labels = ax.get_legend_handles_labels()
fig.legend(handles, labels, frameon=False, ncol=4, fontsize=8,
           loc="upper center", bbox_to_anchor=(0.5, 1.12), columnspacing=1.5)
style(ax)

ax = axes[1]
for i, (cfg, label, color) in enumerate([
        ("dram_only", "DRAM-only", GREEN),
        ("cxl_only", "CXL-only", GRAY),
        ("our_full", "Our-full", BLUE),
        ("weisgut_static_column", "Weisgut-\nstatic", ORANGE)]):
    family = "W3_JOB_REFERENCE" if cfg in ("dram_only", "cxl_only") else "W3_JOB_PRIMARY"
    selected = [r for r in w_rows if r["family"] == family and r["config"] == cfg
                and int(r["seed"]) in (1001, 1002, 1003)]
    assert sorted(int(r["seed"]) for r in selected) == [1001, 1002, 1003], cfg
    xs = [float(r["geomean_avg_ms"]) for r in selected]
    mid = bar_with_range(ax, i, xs, color, width=0.6, mid_fn=st.mean)
    ax.text(i, max(xs) + 2, f"{mid:.2f}", ha="center", va="bottom", fontsize=7)
ax.set_xticks(range(4), ["DRAM-only", "CXL-only", "Our-full", "Weisgut-\nstatic"])
ax.set_ylabel("Geomean latency (ms)")
ax.set_ylim(0, 130)
ax.set_yticks([0, 30, 60, 90, 120])
ax.set_title("(b) JOB, 3.47 GiB policy budget", loc="left", fontsize=9)
style(ax)
save(fig, "fig5_tpch")


# Figure 6: compact secondary adaptation summary.
fig, ax = plt.subplots(figsize=(3.33, 2.45))
names = ["Static plan\nTPC-H", "Static no-plan\nTPC-H", "Static plan\nJOB",
         "Static no-plan\nJOB", "1 s vs 5 s\nTPC-H plan", "5 s vs 1 s\nJOB plan"]
keys = ["D2[TPCH_SF100] static_sem vs adaptive our_full",
        "D2[TPCH_SF100] static_freq vs adaptive our_freq",
        "D2[JOB] static_sem vs adaptive our_full",
        "D2[JOB] static_freq vs adaptive our_freq",
        "D4[TPCH p1 vs p5] our_full", "D4[JOB p5 vs p1] our_full"]
lookup={x["comparison"]:x for x in d_stats}
effects=[lookup[k]["effect_pct"] for k in keys]
cis=[lookup[k]["bootstrap_95_ci_pct"] for k in keys]
err=[[e-c[0] for e,c in zip(effects,cis)], [c[1]-e for e,c in zip(effects,cis)]]
ax.errorbar(range(len(names)), effects, yerr=err, fmt="o", color=BLUE, capsize=2, lw=1)
ax.set_xticks(range(len(names)), names, rotation=28, ha="right")
ax.set_ylabel("Latency difference (%)")
ax.set_ylim(-5, 5)
style(ax, zero=True)
save(fig, "fig6_budget_shift")


# Figure 7: where the plan signal helps.
fig, axes = plt.subplots(2, 1, figsize=(3.33, 4.35))
ax = axes[0]
cfg_order=[("our_full","Plan-aware",BLUE), ("our_freq","No-plan",LIGHT_BLUE),
           ("cost_c3_greedy","Typed-cost",ORANGE), ("dram_only","DRAM-only",GRAY),
           ("cxl_only","CXL-only",LIGHT_GRAY)]
for j,(cfg,label,color) in enumerate(cfg_order):
    xs=[float(r["geomean_avg_ms"]) for r in c_rows if r["config"]==cfg]
    mid = bar_with_range(ax, j, xs, color, label=label, width=0.62, mid_fn=gm)
    ax.text(j, max(xs) + 2, f"{mid:.2f}", ha="center", va="bottom", fontsize=7)
ax.set_xticks(range(5), ["Plan", "No-plan", "Typed\ncost", "DRAM", "CXL"])
ax.set_ylabel("JOB geomean latency (ms)")
ax.set_ylim(0,130)
style(ax)
ax.text(0.5, -0.43, "(a) Primary JOB comparison", transform=ax.transAxes,
        ha="center", va="top", fontsize=8)

ax=axes[1]
client_effects=[]; client_ranges=[]
for client in [1,2]:
    full=sorted((int(r["rep"]),float(r["geomean_ms"])) for r in d_rows
                if r["family"]=="D3_CONCURRENCY" and r["clients"]==str(client) and r["config"]=="our_full")
    freq=sorted((int(r["rep"]),float(r["geomean_ms"])) for r in d_rows
                if r["family"]=="D3_CONCURRENCY" and r["clients"]==str(client) and r["config"]=="our_freq")
    ratios=[100*(a[1]/b[1]-1) for a,b in zip(full,freq)]
    effect=gm([1+r/100 for r in ratios])*100-100
    client_effects.append(-effect)
    client_ranges.append((-max(ratios),-min(ratios)))
core=next(x for x in c_stats if x["comparison"]=="our_full_vs_our_freq")
client_effects.append(-core["effect_pct"])
client_ranges.append((-core["bootstrap_95_ci_pct"][1],-core["bootstrap_95_ci_pct"][0]))
lo=[e-r[0] for e,r in zip(client_effects,client_ranges)]
hi=[r[1]-e for e,r in zip(client_effects,client_ranges)]
ax.errorbar(range(3),client_effects,yerr=[lo,hi],fmt="o",color=BLUE,capsize=3,lw=1.25)
ax.set_xticks(range(3),["1 client","2 clients","10 clients"])
ax.set_ylabel("Plan-aware improvement (%)")
ax.set_ylim(-8,14)
style(ax,zero=True)
ax.text(0.5, -0.33, "(b) Client configurations", transform=ax.transAxes,
        ha="center", va="top", fontsize=8)
save(fig,"fig7_signal")


# Figure 8: secondary component configurations and sensitivity effects. Horizontal bars
# show the aggregate paired effect; whiskers retain the matched-repetition range.
fig, axes = plt.subplots(2, 1, figsize=(3.33, 4.35))
lookup={x["comparison"]:x for x in d_stats}
ax=axes[0]
keys=["D6[JOB] our_column vs our_full","D6[JOB] our_norep vs our_full",
      "D6[TPCH_SF100] our_column vs our_full","D6[TPCH_SF100] our_norep vs our_full"]
labels=["JOB column","JOB no replica","TPC-H column","TPC-H no replica"]
eff=[lookup[k]["effect_pct"] for k in keys]
lo=[]; hi=[]
for i,k in enumerate(keys):
    pairs=list(lookup[k]["raw_pairs_ms"].values())
    raw=[100*(pair[0]/pair[1]-1) for pair in pairs]
    lo.append(eff[i]-min(raw)); hi.append(max(raw)-eff[i])
ax.barh(range(4),eff,xerr=[lo,hi],color=GREEN,height=0.58,capsize=2,
        edgecolor="black", linewidth=0.35, error_kw={"lw":0.8})
ax.set_yticks(range(4),labels)
ax.invert_yaxis()
ax.set_xlabel("Latency vs full (%)")
ax.set_xlim(-4,4)
style_hbar(ax)
ax.text(0.5, -0.43, "(a) Component configurations", transform=ax.transAxes,
        ha="center", va="top", fontsize=8)

ax=axes[1]
keys=["D5 our_w03 vs our_w05_control","D5 our_w07 vs our_w05_control",
      "D5 our_k05 vs our_k1_control","D5 our_k2 vs our_k1_control"]
labels=["Access-type\nweight 0.3", "Access-type\nweight 0.7",
        "Cardinality scale\nkappa = 0.5", "Cardinality scale\nkappa = 2.0"]
eff=[lookup[k]["effect_pct"] for k in keys]
lo=[]; hi=[]
for i,k in enumerate(keys):
    pairs=list(lookup[k]["raw_pairs_ms"].values())
    raw=[100*(pair[0]/pair[1]-1) for pair in pairs]
    lo.append(eff[i]-min(raw)); hi.append(max(raw)-eff[i])
ax.barh(range(4),eff,xerr=[lo,hi],color=PURPLE,height=0.58,capsize=2,
        edgecolor="black", linewidth=0.35, error_kw={"lw":0.8})
ax.set_yticks(range(4),labels)
ax.tick_params(axis="y", labelsize=7)
ax.invert_yaxis()
ax.set_xlabel("Latency vs control (%)")
ax.set_xlim(-2,11)
style_hbar(ax)
ax.text(0.5, -0.43, "(b) JOB sensitivity", transform=ax.transAxes,
        ha="center", va="top", fontsize=8)
save(fig,"fig8_ablation")


# Figure 9: repaired-campaign overhead summary aligned with Table 3.
fig, axes = plt.subplots(2, 1, figsize=(3.33, 3.55))
labels = ["TPC-H\n4.1 GiB", "TPC-H\n8.2 GiB", "TPC-H\n16.4 GiB", "JOB\n3.47 GiB"]
daemon_ratio = [0.594, 0.744, 0.818, 0.0186]
# Audited table values use decimal GB (10**9 bytes), not binary GiB.
promoted_gb = [14.16, 13.67, 16.69, 3.53]
demoted_gb = [1.69, 1.79, 1.70, 0.002]
x = range(len(labels))

ax = axes[0]
ax.bar(x, daemon_ratio, color=BLUE, width=0.62, edgecolor="black", linewidth=0.35)
ax.set_xticks(x, labels)
ax.set_ylabel("Daemon ratio")
ax.set_ylim(0, 0.9)
ax.set_yticks([0, 0.3, 0.6, 0.9])
style(ax)

ax = axes[1]
ax.bar([i - 0.15 for i in x], promoted_gb, width=0.30, color=BLUE,
       label="Promoted", edgecolor="black", linewidth=0.35)
ax.bar([i + 0.15 for i in x], demoted_gb, width=0.30, color=ORANGE,
       label="Demoted", edgecolor="black", linewidth=0.35)
ax.set_xticks(x, labels)
ax.set_ylabel("Logical bytes (GB)")
ax.set_ylim(0, 18)
ax.legend(frameon=False, ncol=2, fontsize=7, loc="upper center",
          bbox_to_anchor=(0.5, 1.18), columnspacing=1.4, handlelength=1.6)
style(ax)
save(fig, "fig9_overhead")


print("Generated Phase-E figures 5--9 from validated Phase C/D and repaired Weisgut summaries.")
