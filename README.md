# Query-Plan-Aware Segment Migration on CXL Memory

Research artifacts for **Query-Plan-Aware Runtime Segment Migration for
In-Memory Databases on CXL Heterogeneous Memory**.

This repository contains the Hyrise implementation, retained experiment
scripts and configurations, result summaries, and paper plotting scripts.

## Repository Layout

| Directory | Contents |
| --- | --- |
| `hyrise/` | Database source, migration plugins, benchmark integration, and supplied third-party dependencies |
| `experiments/` | Experiment scripts, configurations, calibration tables, static-column profiles, and retained results |
| `figures/` | Paper figures in PDF format |
| `reproduce_figures.py` | Entry point for regenerating Figures 4--9 |
| `requirements.txt` | Python plotting dependency |

## Reproduce the Figures

With Python 3.12, run from the repository root:

```sh
python -m pip install -r requirements.txt
python reproduce_figures.py
```

The generated PDFs are written to `figures/`. Figures 1--3 are supplied
diagrams. Figure generation uses the included measurements and does not need
CXL hardware or database datasets.

## Implementation

The main components are in `hyrise/src/`:

- `plugins/semantic_placement_plugin.cpp`: initialization, ranking, background migration, and staging copies.
- `plugins/plan_analyzer.cpp`: logical-plan analysis and segment profiling.
- `plugins/policy_mapper.hpp` and `.cpp`: segment selection and byte-budget handling.
- `plugins/weisgut_static_column_plugin.cpp`: Weisgut-style static column placement baseline.
- `lib/memory/linear_numa_memory_resource.cpp`: NUMA allocation and free-space reuse.
- `benchmark/join_order_benchmark.cpp` and `tpch_benchmark.cpp`: workload entry points.

New benchmark runs require Linux, DRAM/CXL NUMA nodes, the matching Hyrise
build, TPC-H SF100 and JOB/IMDB data. The recorded build uses Clang 17, C++20
and NUMA support. See [experiments/README.md](experiments/README.md) for
experiment inputs and configuration locations.

## Availability

This repository contains the currently available source snapshot and result
materials. The transferred snapshot is missing the root Hyrise `CMakeLists.txt`
and source `resources/` directory; these need to be restored from the original
checkout before a clean database build. Database datasets and compiled binaries
are not included. Plot generation is available independently.

Third-party license files are retained. The original Hyrise root license file
also needs to be restored; no new license is assigned to upstream code here.
