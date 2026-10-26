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

**The current K=64 FP32 Spatz reconstruction has an accepted native grid pair.**
Complete input readiness is **13.565 / 6.904 µs**, and the continuous application
interval is **63.720 / 53.273 µs**, direct/broadcast at 1 GHz. Overall schedule
speedup is **1.196103×**, saving **10.447 µs (16.395%)**. Direct output is
serialized and broadcast output parallel, so this is a comparison of the complete
schedules, not isolated input-broadcast attribution. The repository CLI has now
completed one additional run per variant end to end; every measured duration
matched the original accepted run exactly. The original results remain preserved.
These two observations each do not resolve the intermittent startup timeout or
verify fresh-machine provisioning.

See the [focused results and phase chart](docs/project/SPATZ_GRID_V2_RESULTS.md)
and [repository-owned reproduction workflow](tools/socdaml/spatz_grid_v2/README.md).
The exact accepted application/kernel and parser are packaged in this repository;
the workflow uses the existing isolated native profile without temporary kit or
log-only script dependencies. Raw accepted captures remain preserved externally
under `SoCDAML-native/logs`, with identities/hashes in the results JSON.

The five recovered sources remain scalar FP32 GEMM with **K=256**. They are
preserved alongside the supplemental vector reconstruction, which uses a separate
Spatz-enabled 4×4 profile with four west and four north HBM channels instantiated.
All reconstructed matrix addresses retain west node 0 placement and the source
global-join schedules. This does not claim exact reproduction of the historical
presentation.
See the [source audit](docs/project/SOURCE_AUDIT.md).

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
- `tools/socdaml/spatz_grid_v2/`: packaged native V2 check/build/run/validation workflow.
- `docs/project/SPATZ_GRID_V2_RESULTS.md`: accepted reconstructed pair, boundaries, chart and provenance.
- `core/`, `pulp/`: upstream submodules; retain their revision and license history.

Existing `_checked`, `_bench`, and `_profile` applications, when present, belong
to earlier validation work. Their results do not establish the slide speedups.

## How to continue

Native Apple Silicon bring-up, scalar correctness checks, and the earlier
matched row experiment have already completed. **Do not repeat the scalar phase
profiling milestone for this cleanup.** The current `SoCDAML_MacNative` package
installs these source/documentation changes without rebuilding or running GVSoC.

The reconstructed K=64 grid pair now passes full numerical and completion-boundary
checks. Use the [V2 workflow](tools/socdaml/spatz_grid_v2/README.md) to inspect
dependencies, build either application, validate preserved evidence or regenerate
results/figures. Only an explicit `run` command starts a bounded simulation;
there is no automatic retry or environment rebuild. Historical percentages remain
reference values, not targets for tuning. Earlier reproduction history remains
in [REPRODUCTION.md](docs/project/REPRODUCTION.md) and
[CODEX_STATUS.md](docs/project/CODEX_STATUS.md).

New personal projects are deferred until this reproduction is complete. Keep
them in a separate repository with explicit attribution and independent scope.
