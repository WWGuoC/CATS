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
materials, including the root Hyrise CMake configuration, resource directory,
and dependency declarations. Full benchmark datasets and compiled binaries
are not included. Plot generation is available independently. A clean Linux
database build has not been tested during packaging.

For a Linux build with Clang 17 and the required system dependencies installed:

```sh
cmake -S hyrise -B hyrise/build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=clang-17 -DCMAKE_CXX_COMPILER=clang++-17 \
  -DENABLE_NUMA_SUPPORT=ON
cmake --build hyrise/build --target hyriseBenchmarkJoinOrder \
  hyriseBenchmarkTPCH hyriseSemanticPlacementPlugin \
  hyriseWeisgutStaticColumnPlugin --parallel 8
```

The supplied dependency trees are checked in as ordinary files; the nested
`hyrise/.gitmodules` preserves their original upstream locations. The CMake
file also declares an unused `cpp-btree` interface target whose directory is
absent from this snapshot.

The original Hyrise license is in [hyrise/LICENSE](hyrise/LICENSE).
Third-party license files are retained in their respective directories.
