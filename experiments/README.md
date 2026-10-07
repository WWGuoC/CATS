# Experiment Materials

Existing directory names are preserved so that the plotting scripts can locate
their inputs without changing the measurements or analysis tables.

| Directory | Main contents |
| --- | --- |
| `01_phase_b/source_patches/` | Earlier implementation patches |
| `02_phase_b2/experiment_sources/` | Retained experiment runners, calibration code, and validation scripts |
| `02_phase_b2/calibration/` | Cost-C3 calibration inputs |
| `03_phase_c/` | Primary JOB comparison: campaign plan, per-run configuration and summaries, aggregate statistics |
| `05_phase_d/` | Secondary configuration comparisons and sensitivity results |
| `07_weisgut_static_column/profiles/` | Frozen JOB and TPC-H column profiles |
| `08_weisgut_repair_rerun/` | Repaired overall/static-column campaign and phase-A-only profile |
| `edbt_experiment_pack/` | Retained analysis/validation scripts and schemas |
| `v1_extracted/vldb_experiments_summary/` | Hardware microbenchmark inputs for Figure 4 |

## Run Settings

TPC-H uses SF100 and 22 queries; JOB uses 113 queries. The main comparisons
use ten clients. Typical timed durations are 1200 s for TPC-H and 600 s for
JOB, with migration periods of 5 s and 1 s, respectively. The exact settings,
seeds and command arguments are in each campaign plan and `run_config.json`.

The historical scripts and commands contain paths from the original Linux
machine. Update executable, plugin, dataset, calibration, profile and output
paths for your environment. The original DRAM/CPU NUMA node is 1 and the CXL
node is 2; use the appropriate nodes on your machine. The supplied historical
patches should not be reapplied on top of the later source snapshot.

## Figure Inputs

| Figure | Input |
| --- | --- |
| 4 | Hardware latency and CXL bandwidth summary CSVs |
| 5 | Repaired campaign, matched four-configuration query latency |
| 6 | Phase D frozen-placement and daemon-period summaries |
| 7 | Phase C signal comparison and Phase D client-count results |
| 8 | Phase D component and sensitivity summaries |
| 9 | Migration/replica overhead values retained in the plotting script and result tables |

The figures use retained summaries, not newly executed benchmarks. Campaigns
use different revisions and preparation settings and should be analyzed
separately. The historical final-residency mechanism analysis is omitted
because it counted pending promotions rather than complete residency sets.

To regenerate the paper figures, run `python reproduce_figures.py` from the
repository root. Original experiment collection scripts are included where
available; the package does not supply a single runner for every campaign.
