# SoCDAML: inter-cluster broadcast GEMM experiments

Jerry Yun's SoftHier application experiments compare direct HBM reads with
inter-cluster sharing of GEMM inputs, first across one row and then across a
4×4 cluster grid. The goal of this cleanup is a readable, reproducible account
of the five benchmarks presented in **SoCDAML Presentation**.

The applications build on ETH Zurich / University of Bologna's PULP, SoftHier
SDK, and GVSoC work. This repository does not claim authorship of those systems.
See [upstream documentation](README.upstream.md) and
[contributions](docs/project/CONTRIBUTIONS.md).

## Five experiments

Sources live in `soft_hier/flex_cluster_sdk/app_example/gemm/`.
Coordinates below are **C[y,x]**. All cores participate in global barriers;
the worker count is the number of active DM cores.

| File | Original | Workers | Experiment |
| --- | --- | ---: | --- |
| `single_cluster.c` | `benchmark_1.c` | 1 | Single-cluster GEMM baseline |
| `row_direct.c` | `benchmark_2.c` | 4 | Each row worker fetches A and its B tile |
| `row_broadcast.c` | `benchmark_3.c` | 4 | Fetch A once and broadcast it across row 0 |
| `grid_direct.c` | `benchmark_4.c` | 16 | Each grid worker fetches its A/B strips |
| `grid_broadcast.c` | `benchmark_5.c` | 16 | Row leaders share A; column leaders share B |

[Benchmark notes](soft_hier/flex_cluster_sdk/app_example/gemm/README.md) explain
the transfer geometry, synchronization, and timing boundaries.

## Reproduction status

**Source cleanup is available; the presentation's performance reproduction is
pending.** The five recovered sources use scalar FP32 GEMM with **K=256**.
The slides describe **K=64**, Spatz_4 per cluster, and west+north HBM channels.
The currently validated native configuration disables Spatz and has west HBM
channels only. Merely enabling Spatz does not turn these scalar loops into a
vector kernel. See the [source audit](docs/project/SOURCE_AUDIT.md).

The slide results are historical reference values, not fresh measurements of
this cleanup:

| Comparison | Slide total-time reduction | Equivalent speedup |
| --- | ---: | ---: |
| Row: benchmark 2 → 3 | 24.7% | approximately 1.33× |
| Grid: benchmark 4 → 5 | 35.4% | approximately 1.55× |

Speedup = direct time / broadcast time. Time reduction = 1 − broadcast/direct.
The grid comparison also changes output-store scheduling; it is a comparison
of the complete strategies, not an isolated measurement of broadcast hardware.

## Repository map

- `soft_hier/flex_cluster_sdk/app_example/gemm/`: five cleaned application sources.
- `soft_hier/flex_cluster_sdk/app_example/diagnostics/`: bring-up and SDK examples.
- `soft_hier/flex_cluster_sdk/app_example/experimental/`: unfinished application work.
- `soft_hier/flex_cluster_sdk/runtime/`: existing SDK and allocation/DMA support.
- `soft_hier/flex_cluster/`: simulator architecture and component configuration.
- `docs/project/REPRODUCTION.md`: slide contract and completion criteria.
- `docs/project/SOURCE_AUDIT.md`: precise source differences and cleanup edits.
- `docs/project/reproduction/`: pinned originals, source hashes, and review diff.
- `tools/socdaml/`: existing setup, snapshot, and configuration utilities.
- `core/`, `pulp/`: upstream submodules; retain their revision and license history.

Existing `_checked`, `_bench`, and `_profile` applications, when present, belong
to earlier validation work. Their results do not establish the slide speedups.

## How to continue

Native Apple Silicon bring-up, scalar correctness checks, and the earlier
matched row experiment have already completed. **Do not repeat the scalar phase
profiling milestone for this cleanup.** The current `SoCDAML_MacNative` package
installs these source/documentation changes without rebuilding or running GVSoC.

Before a presentation reproduction run, recover or identify the original Spatz
GEMM implementation, matching build flags, topology, and HBM placement. Reconcile
the source barriers with the slide notes, validate all output elements and DMA
destinations, and then measure the five cases using one documented timing
definition. Follow [REPRODUCTION.md](docs/project/REPRODUCTION.md).

New personal projects are deferred until this reproduction is complete. Keep
them in a separate repository with explicit attribution and independent scope.
