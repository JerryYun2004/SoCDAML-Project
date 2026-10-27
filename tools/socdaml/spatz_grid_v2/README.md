# Native Spatz grid V2 workflow

This packages the accepted K=64 FP32 direct/broadcast reconstruction. It uses
the **existing** `presentation-spatz-v1` profile and native toolchain. There is
no environment installer, model rebuild, automatic patch application or retry.
Only the explicit `run` subcommand starts GVSoC, once, with fixed **300-second /
64-MiB** limits, a continuously drained PTY and concurrent sample attempts at
2/20/45 seconds. `check`, `validate`, `results` and plotting do not simulate;
`build` compiles the applications only.

**End-to-end checkpoint, 2026-10-26:** the repository CLI has now completed one
direct run followed by one broadcast run. Both passed all checks and matched
every original simulated phase exactly. The additional captures and hashes are
recorded separately in
[`packaged-workflow-verification.json`](../../../docs/project/reproduction/spatz_grid_v2/packaged-workflow-verification.json).
The original reference JSON/chart are preserved, not replaced or averaged.

The four accepted target files directly under
`soft_hier/flex_cluster_sdk/app_example/gemm/spatz_grid_v2/` and the modules in
`validated/` are byte-for-byte accepted copies. `direct_checks.py`,
`broadcast_checks.py` and `capture.py` retain accepted helper function bodies
with packaged imports/globals. `provenance/` preserves the original accepted
runner scripts; these historical scripts are **not entrypoints**. The working
workflow imports nothing from a Downloads extraction or a log-directory script.

Attribution remains in the exact source headers. The driver derives from Jerry
Yun's recovered benchmark schedules; the kernel is a documented reconstruction,
not a recovered supervisor implementation. ETH Zurich / University of Bologna,
PULP, SoftHier and GVSoC authorship/licenses remain applicable. Recovered scalar
benchmarks and `docs/project/reproduction/originals/` are not replaced.

## Paths and read-only checks

From the cleanup repository root, set the existing native workspace path:

```bash
native_root="${SOCDAML_NATIVE_ROOT:?Set SOCDAML_NATIVE_ROOT to your existing native workspace}"
repo_root="$PWD"
py="$native_root/venv/bin/python"
runner="$repo_root/tools/socdaml/spatz_grid_v2/reproduce.py"
source "$native_root/env.sh"
export PYTHONDONTWRITEBYTECODE=1
"$py" "$runner" --native-root "$native_root" check
```

`check` verifies target/parser hashes, preserved original hashes, compiler hash,
required executable paths, the full configured profile source, installed model
and dependency snapshots, and the event-only regex allowlist using libc POSIX
basic regexes (the pinned engine's syntax). It does not invoke GVSoC or a compiler.
Run the actual simulator commands from a normal Terminal/outside the agent
sandbox: earlier sandbox filesystem overhead delayed host configuration parsing.

`reference/` stores pinned manifests, accepted ELF/kernel hashes, revisions and
the normalized model-config fingerprint. Captured workspace roots are provenance;
the CLI rebases workspace paths from `--native-root`, never setup paths from a
temporary kit. Binary libraries can still contain workspace-dependent Mach-O
install names; arbitrary relocation or a different model/toolchain is not
certified by this packaging.

## Build either variant using existing tools

Choose a fresh build directory; the command refuses to overwrite one:

```bash
build_parent="$(mktemp -d "$native_root/logs/spatz-grid-v2-package-XXXXXX")"
build_dir="$build_parent/apps"
"$py" "$runner" --native-root "$native_root" build \
  --variant both --output-dir "$build_dir"
```

Use `--variant direct` or `--variant broadcast` to build only that executable.
Both use the same accepted assembly at 0x803f0000. The command saves compile/link
argv, disassembly and build hashes and rejects a kernel differing from accepted
bytes. It does not rebuild or patch the model. GCC flags are the accepted
`rv32imafdv_zfh`, `ilp32d`, `-O3` flags in `reference/compiler.json`.

## Run one selected variant

These are the workflow execution instructions. No simulation ran during the
initial consolidation; the subsequent end-to-end checkpoint used this CLI once
per variant, with the existing packaging build and fresh evidence directories.
Each command below is a separate explicit simulation; there is no pair-run loop:

```bash
"$py" "$runner" --native-root "$native_root" run \
  --variant direct --elf "$build_dir/grid_direct.elf"

"$py" "$runner" --native-root "$native_root" run \
  --variant broadcast --elf "$build_dir/grid_broadcast.elf"
```

An unchanged original accepted ELF can also be supplied explicitly. Other ELFs
require a matching packaged `build.json` and all build-artifact/source guards.
Every launch creates a fresh `native/logs/spatz-grid-v2-<variant>-…` directory
and archive. Limits are fixed; timeout/output-limit/failure preserves evidence
and exits without retry. A normal exit code alone is not a pass. The full parser,
raw DMA and vector boundary checks, model fingerprint and preservation checks
must succeed. Samples can be unavailable/timeout without pausing output draining;
their outcomes are retained. No host duration is used as performance data.

## Validate saved evidence, without running GVSoC

The accepted captures remain under native/logs; neither raw captures nor large
ELFs are duplicated into Git:

```bash
direct="$native_root/logs/presentation-grid-direct-v2-0puwdfmc"
broadcast="$native_root/logs/presentation-grid-broadcast-v2-pty-69p_kpqx"
"$py" "$runner" --native-root "$native_root" validate \
  --variant direct --evidence "$direct"
"$py" "$runner" --native-root "$native_root" validate \
  --variant broadcast --evidence "$broadcast"
```

For a new run, replace `--evidence` with the fresh directory printed by `run`.
Both variants must pass 262,144 word checks and all-worker completion checks.
Offline validation requires saved artifact hashes, source/ELF and runtime config,
and refuses interrupted captures. It does not weaken checks to accept a partial log.

## Regenerate the accepted summary and chart

```bash
results="$repo_root/docs/project/reproduction/spatz_grid_v2"
"$py" "$runner" --native-root "$native_root" results \
  --direct "$direct" --broadcast "$broadcast" \
  --output "$results/regenerated-results.json"
MPLCONFIGDIR="$native_root/build/matplotlib-cache" \
  "$py" "$repo_root/tools/socdaml/spatz_grid_v2/plot_results.py" \
  "$results/regenerated-results.json" --output "$results/regenerated-phase-breakdown"
```

Use a fresh output filename; the CLI refuses to overwrite existing results.
The preserved accepted JSON/chart remain the reference.
The summary is regenerated from strict offline checks of raw accepted logs and
contains evidence/report/archive/ELF hashes. The PNG/SVG chart reads that JSON,
asserts the phase sum equals the continuous interval, and uses simulated ns only.
Plotting uses existing Matplotlib/NumPy; validation/runner logic is standard-library
Python. No package installation is performed.

Targeted packaging checks (no simulation):

```bash
"$py" -m unittest discover -s tools/socdaml/spatz_grid_v2 \
  -p test_packaging.py -v
```

See [accepted results and exact measurement definitions](../../../docs/project/SPATZ_GRID_V2_RESULTS.md).
There are now two accepted observations per variant: the preserved reference
run and one packaged CLI verification. All measured durations matched exactly;
this small check is not a statistical variability estimate. The earlier
intermittent startup timeout remains unexplained. Fresh-machine provisioning
has not been verified; successful runs do not establish the timeout's resolution.

## Separate controlled parallel-output experiment

`direct_parallel` selects the separately attributed experimental driver under
`spatz_grid_v2/experimental/`. It changes only the output selection of the
benchmark-4-derived direct reconstruction to use the accepted broadcast driver's
parallel output implementation, DMA completion wait and final join. It is new
experimental code, not recovered historical benchmark 4. The original `direct`
variant and original accepted results remain intact. Exact-source guards enforce
this one-selection difference; the accepted kernel/parser/model remain pinned.

Compile only the new application using the installed compiler:

```bash
"$py" "$runner" --native-root "$native_root" build --variant direct_parallel
```

Use the fresh path printed by `build` as `experimental_build`. Each of the next
commands explicitly starts one simulation; execute the second only if the first
passes. Both retain fixed 300-second / 64-MiB limits, PTY draining, sparse events,
concurrent sample attempts and no retry. Set `SOCDAML_BROADCAST_ELF` to the
existing pinned broadcast ELF (or one built by this CLI); no fixed build directory
is assumed.

```bash
"$py" "$runner" --native-root "$native_root" run --variant direct_parallel \
  --elf "$experimental_build/grid_direct_parallel.elf"
"$py" "$runner" --native-root "$native_root" run --variant broadcast \
  --elf "${SOCDAML_BROADCAST_ELF:?Set the existing broadcast ELF path}"
```

All existing direct-input numerical, DMA, vector-workload and timing checks apply
to `direct_parallel`. Additional checks require per-cluster output completion
within its local t5..t6 phase and observed overlap of output DMA transactions in
both variants. Validation and validation-only joins remain after t7. Captures
include the runner and orchestration/checker sources, CLI argv/cwd, simulator
argv/environment, model config, target sources, artifact hashes and archive.

For offline comparison, use **new evidence paths** and a **new output file**:

```bash
"$py" "$runner" --native-root "$native_root" results \
  --direct-variant direct_parallel --direct "$experimental_evidence" \
  --broadcast "$controlled_broadcast_evidence" --output "$controlled_results"
"$py" "$repo_root/tools/socdaml/spatz_grid_v2/plot_results.py" \
  "$controlled_results" --output "$controlled_chart_stem"
```

The result writer refuses to overwrite an existing JSON file. Chart labels and
attribution recognize the separate controlled schema. Original accepted JSON,
charts and the 1.965× / 1.196× comparison are preserved. Focused host checks:
`"$py" -m unittest discover -s tools/socdaml/spatz_grid_v2 -p test_output_policy.py -v`.

The controlled pair completed successfully on 2026-10-26, one run each. See
[controlled results, boundaries and limitations](../../../docs/project/SPATZ_GRID_V2_CONTROLLED_OUTPUT.md).
Complete input/application times are 13.550/59.911 µs versus 6.904/53.273 µs;
input/application speedups are 1.963× / 1.125×. Both outputs are parallel,
with output/final-join totals 3.616 / 3.621 µs. Original results remain unchanged.

## Sharing and external dependencies

See the [sharing guide](../../../docs/project/SHARING.md) for the four distinct
levels of reproducibility, patch order and raw-capture requirements. Historical
native setup/build scripts are now preserved in [provenance/native-setup](provenance/native-setup/README.md),
with hashes and exact revision observations. They are archival provenance,
not a supported fresh-machine installer. `reference/model-configuration/` holds
the exact base/Spatz architecture and generated model parameters. Current CLI
commands use neither the external kit nor the archived setup entrypoints.
