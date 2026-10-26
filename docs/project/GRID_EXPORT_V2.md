# K=64 Spatz grid: lossless validation export V2

**2026-10-26:** both direct and corrected event-only broadcast verification now
pass all 262,144 word checks, all-worker vector/DMA completion bounds and
preservation checks. Complete input readiness is **13.565 / 6.904 µs** and
continuous application is **63.720 / 53.273 µs**, direct/broadcast at 1 GHz.
Overall schedule speedup is **1.196103×**, saving **10.447 µs (16.395%)**.
Direct output is serialized and broadcast output parallel, so this is a full
schedule comparison, not isolated input-broadcast attribution. One sample each;
the earlier timeout's cause/resolution remains unproven. See
[CODEX_STATUS.md](CODEX_STATUS.md) for exact commands/evidence and phase details.
All current one-run authorizations have been used. The implementation and
host-check instructions below remain valid.

**Packaged checkpoint:** the repository-owned CLI subsequently passed one new
direct and one new broadcast run, matching all original phase timings exactly.
Use [the repository workflow](../../tools/socdaml/spatz_grid_v2/README.md) and
[results/evidence record](SPATZ_GRID_V2_RESULTS.md). Kit commands below describe
the earlier development checkpoint; the original results are not overwritten.
Fresh-machine provisioning and the intermittent timeout remain unverified.

Working kit:
`/Users/jerryyun/projects/SoCDAML-native/kits/grid-export-v2/SoCDAML_MacNative`

The Downloads kit, installed Spatz profile, prior evidence and accepted
single-cluster result remain unchanged. This working kit is a separate fork;
its historical documentation does not authorize another benchmark campaign.
The reusable patch is `tools/socdaml/grid-export-v2.patch` in the cleanup repo.

## What changed

`reproduction/presentation_grid/grid.c` emits all A, B, local-C and HBM-C words
after INTERVAL_END, using 192-word Base64 blocks. Explicit little-endian
serialization preserves raw bits, including signed zero and NaN payloads. The
payload uses the SDK's existing `_putchar` port; only block metadata uses
`printf`. Initialization, DMA schedules, compute, joins, timing probes and HBM
readback remain unchanged. No checksum replaces the full-word evidence.

Each record is:

```text
[SOCDAML_SPATZ_GRID_V2] WORDS64 cid=0 kind=A index=0 count=192 <Base64 payload>
```

The final block has 64 words. There are 22 blocks per 4,096-word tile, 64 tiles
and **262,144 checked words per variant**. The other records retain their
fields, under the V2 tag and `source_scheduled_spatz_grid_v2` protocol.

`reproduction/word_export.py` strictly decodes blocks and expands them into
eight-word records for the existing numerical, DMA, phase and vector checker.
It rejects incomplete, duplicate, reordered, oversized, malformed, mixed-V1,
interleaved and noncanonical records. A partial capture cannot pass. The V2
checker intentionally rejects archived V1 streams; use their archived checker.

| Validation-word export per variant | V1 | V2 |
|---|---:|---:|
| Words retained | 262,144 | 262,144 |
| Formatter calls | 32,768 | 1,408 |
| ASCII bytes, including word-record metadata/newlines | 4,164,864 | 1,493,968 |

Word-export bytes fall **64.129%** and formatter calls fall **95.703%**. Other
protocol/vector traces are excluded from these byte counts. These counts do
not establish simulated performance or guarantee completion within 60 seconds.

## Reproduce the host checks

From the native workspace, using the existing SDK-aware host compiler wrapper:

```bash
cd /Users/jerryyun/projects/SoCDAML-native
export CC="$PWD/build/toolchain-host/bin/cc"
venv/bin/python -m unittest discover \
  -s kits/grid-export-v2/SoCDAML_MacNative/reproduction/tests \
  -p test_word_export.py -v
venv/bin/python -m unittest discover \
  -s kits/grid-export-v2/SoCDAML_MacNative/reproduction/tests \
  -p test_presentation_grid.py -v
```

All **15 tests passed**. The actual C encoder is compared with Python's Base64
implementation for all tail lengths and raw bit patterns. The 48-thread
functional fixture runs direct/broadcast at O0 and O3, checks every word and
DMA schedule, asserts exports occur outside timing, and detects missing input,
broken broadcast, corrupt output, delayed remote completion and bad vector
evidence. Its timers are synthetic and its GEMM is a C stand-in, so these runs
cannot validate the native ISS/DMA model or yield performance results.

The fixture now supports macOS with a mutex/condition-variable barrier and a
4-GiB host address bias. Address mappings use non-destructive hints and assert
the returned address. DMA-event output removes the host-only bias, retaining
the original schedule/address expectations. Linux keeps its existing mappings.

## Compile only, without rebuilding the simulator

```bash
cd /Users/jerryyun/projects/SoCDAML-native
source env.sh
venv/bin/python \
  kits/grid-export-v2/SoCDAML_MacNative/reproduction/prepare_grid_export.py \
  "$PWD" "$PWD/logs/presentation-grid-v6nsjj3k"
```

This command only compiles two ELFs and their shared kernel object using the
existing compiler. It verifies prior artifacts, dependencies, native/profile
sources and installed libraries before/after, compares the measured t0–t7
source with V1, and requires both linked kernels to match the prior kernel
bytes at `0x803f0000`. It never calls a simulator or simulator build.

Verified build evidence:
`SoCDAML-native/logs/presentation-grid-export-v2-build-n3h9jxb7`
(with an adjacent `.tar.gz`). The kernel SHA-256 remains
`c8a8a2f32342ef6cb19921a5fb3f25cb8de9467b283562d2a54be7e086c7e3d6`.
Recompiling the driver may change instruction layout; identical measured source
and shared-kernel bytes do not prove identical driver instruction addresses.

## Remaining step

Consolidate the accepted pair's provenance, reproducible commands and measurement
definitions into the reproduction results README. Do not rerun the noisy exec
diagnostic or launch another simulation under the used one-run authorizations.
The historical 1,800-second and earlier broadcast timeout remain unexplained.
