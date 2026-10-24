# Presentation reproduction contract

The immediate goal is a clean, attributable reproduction of Jerry Yun's five
GEMM experiments and their presentation results. Additional personal projects
and further profiling of the earlier K=256 scalar test are deferred.

## Configuration described by the presentation

| Property | Presentation | Recovered source / current native setup |
| --- | --- | --- |
| Grid | 4×4 clusters | 4×4 clusters |
| L1 | 1 MiB per cluster | 1 MiB per cluster |
| Compute resources | 3 Snitch cores + Spatz_4 per cluster | 3 cores; Spatz attachment list empty |
| HBM placement | 4 west + 4 north channels | 4 west channels |
| GEMM reduction dimension | K=64 | K=256 in all five originals |
| GEMM implementation | Presentation describes Spatz | Scalar FP32 loop on DM core |
| Row input/compute overlap | Slide 11 notes say no inter-cluster barrier before compute | Original row sources synchronize before compute |

The four-channel claim is checked against both the source and installed
architecture hashes in the uploaded native benchmark metadata. The table does
not establish which historical revision generated the slides.

## Experiment matrix to reproduce

| ID | A | B | C | Workers | Strategy |
| --- | --- | --- | --- | ---: | --- |
| 1 | 64×64 | 64×64 | 64×64 | 1 | Single cluster |
| 2 | 64×64 | 64×256 | 64×256 | 4 | Direct A and B reads |
| 3 | 64×64 | 64×256 | 64×256 | 4 | Row broadcast of A |
| 4 | 256×64 | 64×256 | 256×256 | 16 | Direct A and B reads |
| 5 | 256×64 | 64×256 | 256×256 | 16 | Row broadcast of A, column broadcast of B |

Within each comparison pair, inputs, math, active workers, output ownership,
compiler, simulator revision, hardware configuration and timing definitions
must agree. Retain intended differences in communication and scheduling and
state them explicitly. Benchmark 4 versus 5 currently includes a change from
serialized to parallel output stores.

## Historical result targets

Slide 16 reports total-time reductions of 24.7% (2→3) and 35.4% (4→5).
These correspond to approximately 1.328× and 1.548× speedups.
Slide 17 reports reductions of 66.2% and 72.8% with compute excluded.
Do not confuse reduction percentages, transfer counts, and speedup ratios.

The plot values are images rather than embedded raw chart data. Rounded labels
are reference targets, not exact acceptance thresholds. Do not tune or alter
workloads to force these ratios.

## Next execution milestone

1. Identify the historical compute kernel and configuration. Record its source
   revision, compiler/build flags, Spatz attachment and width, HBM address-to-
   channel mapping, and the timing boundaries used for the charts. Adding
   north HBM nodes alone does not place data in them; changing K alone does not
   reproduce the vector workload.
2. Resolve the row overlap discrepancy against that source. Keep changes from
   the recovered benchmarks in an explicit diff. If the historical kernel is
   unavailable, label a new implementation a reconstruction, not an exact
   reproduction, and report its differences.
3. Validate all five programs: A/B delivery to every worker, every C element
   against an independent reference, and final HBM output placement. A checksum
   or process exit status alone is insufficient. Perform detailed dumps outside
   measured regions and avoid diagnostic barriers inside the execution schedule.
4. Run one correctness-checked sample of each case before a repeated sweep.
   Capture source/config/ELF hashes, compiler version and flags, simulator
   revision, numerical error, simulated nanoseconds, and host runtime separately.
5. Report direct/broadcast ratios for row and grid with the same timing scope.
   Separate compute, movement and waiting only where the timing method supports
   that decomposition. Preserve raw logs and disclose gaps relative to the slides.

The native compatibility layer has passed scalar tests; its Spatz execution
and timing equivalence to the historical Linux setup have not been established.
These are reproduction dependencies, not a reason to repeat scalar profiling.

## Evidence already available

Native single-cluster and corrected row correctness checks have passed in the
author's runs. The later matched K=256 scalar row experiment reported
9,482,705 ns direct and 9,480,632 ns broadcast, repeated identically three times.
It changed the schedule and used the configuration listed above; it is retained
as earlier validation evidence and is not a test of the slide speedup claim.

The source cleanup has host functional checks, not new GVSoC results. Fresh
presentation reproduction results remain **pending**.
