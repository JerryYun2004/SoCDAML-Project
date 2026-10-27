# SoCDAML: inter-cluster GEMM data movement

Jerry Yun's course-project benchmarks compare direct HBM input reads with
inter-cluster input sharing, across one row and a 4×4 cluster grid. His original
benchmark contributions are the five application schedules below. Project work
also includes helpers for common free-region discovery and inter-cluster
allocation/DMA.
[Contribution boundaries](docs/project/CONTRIBUTIONS.md) distinguish adapted code
from original work. This repository also records a later, validated Spatz
reconstruction and its measurement evidence.

The architecture models, SDK/runtime, GVSoC, SoftHier and PULP infrastructure
come from their upstream authors, including ETH Zurich / University of Bologna.
Upstream file credits and licenses remain applicable; this is not a claim of
ownership of that infrastructure. See [upstream documentation](README.upstream.md).

## Sources and scope

The five recovered scalar FP32 benchmarks use K=256. Their exact originals and
hashes are in [reproduction/originals](docs/project/reproduction/originals/);
[cleaned application sources](soft_hier/flex_cluster_sdk/app_example/gemm/README.md)
retain the following mapping:

| Original | Cleaned source | Schedule |
|---|---|---|
| benchmark 1 | single_cluster.c | One-cluster baseline |
| benchmark 2 | row_direct.c | Four workers fetch input directly |
| benchmark 3 | row_broadcast.c | Row workers share A |
| benchmark 4 | grid_direct.c | Sixteen workers fetch A/B directly |
| benchmark 5 | grid_broadcast.c | Row leaders share A; column leaders share B |

The separate [Spatz V2 applications](soft_hier/flex_cluster_sdk/app_example/gemm/spatz_grid_v2/README.md)
reconstruct K=64 FP32 GEMM (256×64 × 64×256), 16 clusters at 1 GHz. The shared
vector kernel and experimental direct/parallel-output driver are new
reconstruction work, **not recovered historical code**. Historical slide results
are not claimed as freshly reproduced. See the [source audit](docs/project/SOURCE_AUDIT.md).

## Accepted native comparisons

Times are simulated µs. Speedup = direct / broadcast. Validation is excluded.

| Comparison | Direct / broadcast output | Input readiness | Application | Input / application speedup |
|---|---|---:|---:|---:|
| [Original complete schedules](docs/project/SPATZ_GRID_V2_RESULTS.md) | Serialized / parallel | 13.565 / 6.904 | 63.720 / 53.273 | 1.965× / 1.196× |
| [Matched parallel output](docs/project/SPATZ_GRID_V2_CONTROLLED_OUTPUT.md) | Parallel / parallel | 13.550 / 6.904 | 59.911 / 53.273 | 1.963× / 1.125× |

The original comparison includes the output-policy change. The controlled pair
saves **6.638 µs (11.08%)** overall: 6.646 µs input saving minus 3 ns additional
compute/synchronization and 5 ns output/synchronization. Its direct input policy
is unchanged; the exact cause of its 15-cycle readiness difference remains
unresolved. The reports link full-precision JSON, charts and evidence hashes.

There are two accepted observations each for original direct/broadcast, and one
for each controlled-pair variant. These are small checks, not variability
estimates. The intermittent timeout remains unexplained; fresh-machine
provisioning and relocation remain unverified.

## Inspect or reproduce

- **Inspect results:** source, reports, JSON and charts are tracked here; no native
  environment or raw traces are needed to read them.
- **Revalidate offline:** obtain the named raw evidence directories/archives;
  they are preserved outside Git, not distributed by this repository.
- **Execute:** use the existing pinned Apple Silicon native environment and the
  [repository CLI](tools/socdaml/spatz_grid_v2/README.md). Only `run` simulates;
  each launch is bounded to 300 seconds / 64 MiB without retries.
- **Provision a new machine:** setup history, exact revisions, compatibility
  order, configuration and remaining gaps are in the
  [dependency and sharing guide](docs/project/SHARING.md). This is not a verified installer.

[CODEX_STATUS.md](docs/project/CODEX_STATUS.md) retains the detailed handoff.
Upstream `core` and `pulp` remain submodules; build products, toolchains and large
raw traces are intentionally excluded from publication checkpoints.
