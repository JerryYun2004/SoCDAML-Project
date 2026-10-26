# Codex handoff

## 2026-10-25 — native K=64 Spatz grid evidence review

Reviewed the existing Apple Silicon workspace and kit at
`~/Downloads/socdaml-grid-events.rgwyza/SoCDAML_MacNative` without rebuilding,
changing runtime code, or launching a simulation. Existing dirty cleanup files
and submodules remain preserved. The native workspace root is not a Git repo.
No applicable AGENTS.md was found in the workspace ancestors or documentation
tree. The accepted single-cluster checkpoint was not rerun.

### Finding

`SoCDAML-native/logs/presentation-grid-diagnose-w1kzp95w` **passed the application
interval boundary and stopped during untimed validation export**, at its deliberate
60-second capture limit. It does not demonstrate a startup deadlock or a full
benchmark pass. The original 1,800-second timeout remains unexplained.

Evidence in that directory, with one-based `trace.log` line numbers:

- `report.json`: capture limit 60 seconds / 64 MiB, 2,146,373 bytes collected,
  intentional exit -9, `timing_accepted: false`. All 16 listed artifact hashes
  verify; corresponding archive members and archived report match the directory.
- Initialization completed all 256 A, 64 B, and 256 C-poison row transfers.
  Raw trace records **625 DMA completions**: 576 initialization, 32 application
  input transfers, 16 application output transfers, and one validation readback.
  Cluster 0 has 580 completions; every other cluster has three.
- `trace.log:2616`: BEGIN text exists but is interleaved with decoder output.
  `trace.log:3188` records cluster 0's completed 2D B gather. Every cluster
  completes A/B input and output. The final application's output DMA, cluster 15,
  completes at 1,521,817 simulated ns (`trace.log:9998`).
- Cluster 0 records exactly 1,024 `vle32.v`, 64 `vfmul.vf`, 4,032 `vfmacc.vf`,
  and 64 `vse32.v` completions. Its last vector event is at `trace.log:9212`.
  Other clusters' vector traces were not enabled; no all-worker timing claim.
- `trace.log:10157`: intact INTERVAL_END. `grid.c` places this after t[7], final
  application synchronization, and a validation-only barrier. `grid.c:229`
  onward exports untimed records. BEGIN/COMPUTE/TIMES are partly interleaved;
  do not reconstruct accepted timings from these fragments.
- `trace.log:12889`: cluster 0 HBM-C validation readback DMA completes.
  `trace.log:13008` (last line): cluster 0 prints HBM_C indices 848–855.
  Worker 0 validation export remains incomplete; workers 1–15 and final END
  have not been exported. `grid_direct.dump:2204` identifies `print_words`,
  whose loop calls `printf_` through index 4095.
- Independently checked 13,136 words from intact cluster-0 A/B/C/HBM_C records
  against integer-pattern FP32 references: zero mismatches. This is partial
  validation only; the first A record is interleaved and was excluded.
- `launcher-sample.txt` call graph at approximately 30 host seconds shows
  SystemC coroutine scheduling and active ISS execution, including integer
  offload and memory/fetch operations. It does not show the earlier diagnostic's
  blocked stdout write. One stack sample cannot prove uninterrupted progress.

### Reporting defect and changes

The existing `trace-summary.json` undercounts DMA: its parser expects adjacent
`src`, `dst`, `size` fields, while 2D records insert stride/repeat fields. It
therefore omits all 33 2D completions and reports 592 rather than 625. Its marker
list also misses interleaved protocol text. The raw evidence is preserved.

No GEMM, SDK, compiler, native compatibility patch, machine configuration,
benchmark runner, or original kit file was changed: the evidence supports no
runtime repair. Added this handoff and the separate corrected review at
`SoCDAML-native/logs/presentation-grid-review-20261025/review.json`. No new
diagnostic simulation was needed for this review; the allowance remains unused.
No grid performance or speedup result is accepted.

### Next single action

Run **one** diagnostic of the existing direct ELF under the original launch
conditions (`--trace=spatz`, regular-file output), bounded to **60 seconds and
64 MiB**, with stack sampling concurrent with capture. Compare that launch
against the progressing PTY/event capture, retaining model/ELF hashes. This
distinguishes original launch/trace/output behavior before any runtime fix.
Do not invoke the full pair runner, rebuild, increase limits, or treat this
diagnostic as performance evidence. Stop if it repeats failure without new
evidence. Broadcast and full correctness/timing validation remain pending.

## 2026-10-25 — bounded original-launch replay completed

This update supersedes the next-action instruction above. **Both permitted new
diagnostic simulations are now used.** Each ran for 60 seconds with a hard
64-MiB regular-file size limit. No full benchmark campaign or build ran.

### Experiments and findings

1. `SoCDAML-native/logs/presentation-grid-original-launch-29ohh9lu` replayed the
   original `--trace=spatz --trace-level=debug` command and regular-file output
   against the original direct ELF. It stopped at 60.004 host seconds with
   1,082 bytes, no BEGIN/vector events, and no simulated progress established.
   The automatic 30-second sampler could not inspect processes in the sandbox.
   An explicitly permitted external sample at about 41 seconds was retained as
   `launcher-sample-authorized.txt`. Its call graph (lines 26–57 onward) places
   the launcher in `requires_systemc` → JSON configuration parsing →
   `json_my_stod` → repeated `std::locale("C")` construction/timezone reads,
   **before simulation initialization**. This differs from the historical
   1,800-second run, which had recorded model reset messages.
2. A host-only 500-number probe using the existing compiler/SDK wrapper measured
   named-C-locale parsing at 2.57575 host seconds inside the sandbox versus
   0.0206571 seconds outside. Within the sandbox, explicit `TZ=UTC` took 1.63828
   seconds and `std::locale::classic()` took 0.0000905 seconds. Every case parsed
   the same sum, 61,625. These are single host diagnostic observations; they are
   neither simulated times nor GEMM performance results. No locale patch was
   applied to installed code.
3. Hypothesis for the final allowed diagnostic: sandbox overhead was preventing
   the first replay from reaching the behavior under investigation. Replayed
   the **same script, original launch command, ELF and installed model outside
   the sandbox**, with the same 60-second / 64-MiB limits and concurrent sampling.
   Evidence: `SoCDAML-native/logs/presentation-grid-original-launch-21zacurh`.
   It captured 17,295,059 bytes in 60.074 host seconds:
   - `trace.log:1365`: intact BEGIN.
   - Every cluster recorded the expected 5,184 vector completions: 1,024 loads,
     64 multiplies, 4,032 FMAs, and 64 stores, totaling **82,944** completions.
   - `trace.log:84310`: intact INTERVAL_END; application execution reached its
     final synchronization. Worker-0 COMPUTE and TIMES records follow intact.
   - The log grew at each 10-second checkpoint. At termination, it was partway
     through worker-0 HBM_C validation printing: 210 complete eight-word records
     (indices 0–1679), followed by an incomplete next record at line 86062.
   - All 13,968 exported intact A/B/C/HBM_C words checked exactly against an
     independent integer-pattern FP32 reference. Other workers' exported
     numerical evidence and final END remain absent.
   - `launcher-sample.txt:23` onward shows active SystemC/ISS execution at the
     concurrent 30-second sample, with no JSON/locale stack. A single sampled
     write frame is not evidence of blocked output.

The original trace filter and regular-file output **can progress**. Neither a
target startup deadlock nor a GEMM/model defect was reproduced. Sandbox-specific
host parsing overhead explains the first replay's lack of progress; it does
not establish the cause of the historical 1,800-second timeout. Full grid
correctness, all-worker completion-envelope checks and broadcast remain pending.
No timing or speedup is accepted from these deliberately interrupted captures.

### Changes and preservation

Added the bounded harness at `SoCDAML-native/logs/grid_original_launch_diagnose.py`.
It reuses the existing toolchain environment, original ELF and installed model;
enforces the file limit with `RLIMIT_FSIZE`; samples concurrently; kills only
its disposable process group; and archives evidence with SHA-256 manifests.
Invocation is `source env.sh` then
`venv/bin/python logs/grid_original_launch_diagnose.py` from the native workspace.
**Do not invoke it again under the exhausted current diagnostic allowance.**

Both captures verified original and isolated source manifests, installed model,
dependencies and prior artifact hashes before/after execution. Their 11 listed
artifacts each and corresponding archive members verify. The second launch
regenerated ordinary runtime configuration files, as expected; a copy of its
post-capture JSON configuration is retained in the review directory. No target,
SDK, model source, installed library, native compatibility patch or accepted
checkpoint was changed. Existing cleanup edits and dirty submodules remain.

Detailed review and retained host-probe source:
`SoCDAML-native/logs/presentation-grid-original-launch-review-20261025/`.

### Next single action

Prepare and host-test a compact **post-interval full-word validation export**
that retains every numerical/transport check and the current timing boundaries.
Text export is still incomplete after each bounded capture; reduce that overhead
before another simulation. Do not reduce validation coverage or infer a pass
from the partial words. No further simulations are authorized within the initial
two-run budget, and the historical timeout's cause remains unresolved.

## 2026-10-25 — export V2 prepared and host-tested, no simulation

Completed the next offline step in a separate working kit:
`SoCDAML-native/kits/grid-export-v2/SoCDAML_MacNative`.
See [GRID_EXPORT_V2.md](GRID_EXPORT_V2.md) for exact commands and limitations.
The original Downloads kit still verifies all 158 manifest entries.

### Implementation

- Added a raw-bit-preserving, little-endian Base64 C encoder and 192-word
  `WORDS64` records, emitted only after INTERVAL_END through the existing SDK
  character port. Every variant still exports all **262,144 validation words**.
  Initialization, DMA/readback, schedules, required synchronization and timing
  probes are retained; no checksum substitutes for full-word validation.
- Added a strict host decoder which expands V2 blocks for the existing
  numerical, DMA, phase and vector-completion checker. It rejects incomplete,
  reordered, duplicate, mixed-version, interleaved and noncanonical evidence.
  The protocol/tag is explicitly V2; V1 logs retain their archived V1 checker.
- Validation-word text falls from 4,164,864 to 1,493,968 bytes (**64.129%**),
  and formatting calls fall from 32,768 to 1,408 (**95.703%**). These are exact
  output/call counts, excluding other trace records; export time is unmeasured.
- Ported the 48-thread test fixture to macOS using a reusable condition-variable
  barrier and non-destructive mappings above 4 GiB. This changes host fixtures
  only; target addresses and machine configuration are unchanged.
- Added `reproduction/prepare_grid_export.py`, a compile-only command with
  prior/model/dependency/source preservation checks. It never launches or
  rebuilds a simulator. A reusable patch against the original kit is retained
  at `tools/socdaml/grid-export-v2.patch` in the cleanup repo; its dry-run applies
  cleanly. The working kit has its own updated SHA256SUMS/provenance and docs.

### Verification

All **15 tests passed on this Mac**: ten grid tests and five codec/framing tests.
Direct/broadcast each run with 48 host threads at O0 and O3, checking all words,
DMA order/addresses/strides/masks, phase accounting and output placement outside
timing. Fault injection still detects missing input, broken broadcast, corrupt
output, delayed remote completion and bad/missing vector evidence. The actual
C encoder matches Python Base64 for all tail cases, arbitrary bytes, signed
zero and NaN payloads; truncation and malformed frames are rejected.

Both V2 RISC-V ELFs compile using the existing toolchain. Final reproducible
build-only evidence is `SoCDAML-native/logs/presentation-grid-export-v2-build-n3h9jxb7`
and its adjacent archive. Both linked kernels match the prior accepted bytes
at 0x803f0000, SHA-256
`c8a8a2f32342ef6cb19921a5fb3f25cb8de9467b283562d2a54be7e086c7e3d6`.
Measured t0–t7 source text is identical; recompilation can still change driver
instruction layout. Preservation checks passed for original/profile source,
installed libraries, dependencies and prior artifacts. An earlier ad-hoc build
is retained separately as `presentation-grid-export-v2-build-o_fs7paw`.

**No new simulation ran.** Host timers are synthetic and host GEMM is a C
stand-in. Native V2 numerical completion, export duration and timing remain
unverified. No grid result or speedup is accepted; the original timeout remains
unexplained. Existing dirty cleanup files/submodules and prior evidence remain.

### Next single action

After the user renews the exhausted simulation allowance, verify the prepared
**direct V2 ELF** in one bounded run outside the sandbox, initially retaining
60 seconds / 64 MiB. Use the strict V2 checker; require every numerical record
and worker completion envelope before accepting timing. Preserve partial
evidence and stop at the limits without automatically extending them. Broadcast
and a full benchmark campaign remain pending.

## 2026-10-26 — one authorized direct-V2 native verification PASSED

This update supersedes the earlier pending-direct instructions. The user
explicitly authorized **one** new direct simulation, bounded to **300 host
seconds / 64 MiB**. That allowance has been used once; no retry, broadcast,
environment rebuild, target rebuild or repeated host tests ran. Work remained
within the two SoCDAML workspaces; no Croc/CP3F files were accessed or changed.

### Exact command and evidence

From `/Users/jerryyun/projects/SoCDAML-native`, outside the sandbox to avoid the
previously identified host configuration-parsing slowdown:

```bash
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/verify_grid_direct_v2.py
```

The new bounded harness launches only the copied, unchanged prepared direct ELF.
It uses a hard `RLIMIT_FSIZE=67108864` capture-file limit and a 300-second wall
deadline, continuously observes completion, and never retries. Its exact child
command (working directory `profiles/presentation-spatz-v1/source`) was:

```bash
/Users/jerryyun/projects/SoCDAML-native/profiles/presentation-spatz-v1/install/bin/gvsoc \
  --target=pulp.chips.flex_cluster.flex_cluster \
  --binary /Users/jerryyun/projects/SoCDAML-native/logs/presentation-grid-direct-v2-0puwdfmc/grid_direct.elf \
  run --trace-level=debug \
  '--trace=cluster_[0-9][0-9]*/pe0/spatz/trace$' \
  '--trace=cluster_[0-9][0-9]*/idma/fe/trace$'
```

Evidence directory:
`SoCDAML-native/logs/presentation-grid-direct-v2-0puwdfmc`
and adjacent `.tar.gz`. Key files are `report.json`, `verification.json`,
`readout.json`, `progress.json`, `runtime-config.json`, the unchanged ELF,
source/disassembly, checker copies, harness and `trace.log`. All final archive
members match the 24 recorded artifact hashes; the archived report also matches.

The tool exited normally with code 0 after 59.387 host seconds, capturing
18,642,725 bytes, within both limits. **Host duration is not a performance
measurement.** Acceptance is based on the checks below, not the exit code.

### Required verification

1. The measured application interval completed. `trace.log:1205` is BEGIN,
   `trace.log:84198` is INTERVAL_END, and `trace.log:85703` is final END. The
   master timestamps are `1457894 1471367 1471459 1471459 1471459 1514204
   1521525 1521614`; their adjacent phases sum exactly to t7−t0.
2. The strict V2 decoder/checker accepted all **262,144 raw validation words**:
   A, B, local C and HBM C for all 16 clusters, with no missing/duplicate or
   malformed records and exact reference agreement. Final HBM output hash is
   recorded in `verification.json`.
3. Every cluster executed exactly 1,024 vector loads, 64 vector multiplies,
   4,032 vector FMAs and 64 vector stores: **82,944 operations** in total.
   Every vector event's start/end lies inside its worker call interval. Each
   worker's last vector completion precedes its end probe by **five cycles**;
   all compute calls complete before the master output-handoff boundary.
4. All validation records are after INTERVAL_END and before END. The original
   source placement remains after t7 and its validation-only barrier. All 16
   validation readback DMAs start after every worker's t7. The 640 DMA
   completions comprise 576 initialization transfers, 48 application transfers
   and 16 validation readbacks; application DMA addresses, lengths, strides,
   transaction IDs and completion-within-probe bounds pass independent checks.
5. The ELF matches prepared-build SHA-256
   `0116809c593e837396d53eef079194c2932ba0048be596786535fd6e0da304be`.
   Its shared GEMM bytes still match the prior accepted kernel at 0x803f0000:
   `c8a8a2f32342ef6cb19921a5fb3f25cb8de9467b283562d2a54be7e086c7e3d6`.
   Original/profile source manifests, installed model, dependencies, prior
   artifacts and prepared-build artifacts verify unchanged before/after.

### Accepted direct-only simulated timing

The archived runtime configuration confirms **1 GHz**, so one cycle is one ns.
The continuous application interval is **63,720 cycles = 63.720 µs**, from
t0=1,457,894 to t7=1,521,614 cycles. Setup/allocation and all validation are
excluded; required application synchronization and completed DATA-OUT are included.

| Adjacent master phase | Cycles / simulated ns | µs |
|---|---:|---:|
| HBM input schedule, including scheduling barriers | 13,473 | 13.473 |
| Explicit input completion join | 92 | 0.092 |
| Broadcast schedule / broadcast join (absent in direct) | 0 | 0.000 |
| Compute plus required compute join | 42,745 | 42.745 |
| Serialized output schedule, including scheduling barriers | 7,321 | 7.321 |
| Final output synchronization | 89 | 0.089 |
| **Continuous application total** | **63,720** | **63.720** |

**DATA-IN completion is distinct from synchronization:** actual first DMA start
to actual last input completion spans **13,259 ns (13.259 µs)**, including
inter-transfer schedule gaps. The issue/wait probe envelope is 13,275 cycles;
it also includes software issue/wait overhead. The last actual input completion
is at 1,471,276 ns, or **13,382 ns after t0**. Then 91 ns of schedule completion
bookkeeping and the explicit 92-ns input join reach input-ready t4, for a total
input stage of **13,565 ns (13.565 µs)**. Scheduling barriers inside the input
schedule have not been isolated as pure DMA/bus service time.

Every worker's core-0 compute call is **42,654 cycles (42.654 µs)**. Its wait
through the compute join is 84–92 cycles; worker 0 waits **88 cycles**. For
worker 0, 3 cycles before the call + 42,654 call cycles + 88 join cycles explain
the master 42,745-cycle compute phase. These worker figures are explanatory
and must not be added to the already inclusive phase total. The last actual
application output DMA completes at 1,521,426 ns, before final t7=1,521,614.

This is one accepted **direct program** checkpoint. No broadcast comparison,
speedup, isolated broadcast attribution, variability estimate or exact historical
slide equivalence is established. The original 1,800-second timeout's cause
remains unresolved.

### Next single action

Obtain authorization for one bounded **broadcast-V2** verification using its
already prepared ELF, the same installed model, strict checker and completion
traces. Match the 300-second / 64-MiB limits and measurement scope before any
direct-versus-broadcast comparison. Do not launch another simulation under the
now-used direct-only authorization.

## 2026-10-26 — authorized matching broadcast-V2 run reached 300-second limit

The user authorized one matching broadcast-only simulation with **300 seconds /
64 MiB**. It was launched once and stopped at the time limit. **No retry,
direct rerun, rebuild or completed host-test repetition occurred.** The accepted
direct evidence at `logs/presentation-grid-direct-v2-0puwdfmc/` and its archive
remain unchanged. No unrelated project files were accessed or changed.

### Matching checks before launch

- Prepared broadcast ELF: `logs/presentation-grid-export-v2-build-n3h9jxb7/grid_broadcast.elf`,
  SHA-256 `3714d21b60f7091147ca384bcfd4b85aff9a7e3211e1870a0406fac963618544`.
  It was copied, not recompiled. Source, compiler/flags, linker, kernel source,
  installed-model manifest, dependencies and configured-source manifest match
  the accepted direct provenance.
- Identical FP32 A[256][64] × B[64][256] → C[256][256], per-worker 64³ work,
  4×4 grid, three cores/cluster, Spatz configuration and **1-GHz clock**. The
  current runtime configuration exactly matched archived direct before launch.
- Actual delivered direct A/B bits match the shared V2 input reference, with
  SHA-256 `35d2f3b8a0671453efcef31dd5b50583edaaa7edfed0f7c994746fe98937e78e`.
  Broadcast uses the same initialization/input generator. Actual broadcast
  input delivery cannot be validated because no export was reached.
- Shared fixed-address kernel bytes match direct and the prior kernel,
  SHA-256 `c8a8a2f32342ef6cb19921a5fb3f25cb8de9467b283562d2a54be7e086c7e3d6`.
  Common driver source and timing/validation boundaries are identical; the
  prepared build flag selects the surviving broadcast schedule.
- **Direct output remains serialized; broadcast output is parallel.** Any
  future overall speedup will compare those two complete schedules, rather
  than isolate input broadcasting.

### Exact command and retained evidence

From `/Users/jerryyun/projects/SoCDAML-native`, outside the sandbox:

```bash
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/verify_grid_broadcast_v2.py
```

The harness uses the same installed launcher and all-worker vector/DMA traces
as direct, a 300-second deadline and hard 67,108,864-byte `RLIMIT_FSIZE` limit.
Its exact child command, working directory
`profiles/presentation-spatz-v1/source`, was:

```bash
/Users/jerryyun/projects/SoCDAML-native/profiles/presentation-spatz-v1/install/bin/gvsoc \
  --target=pulp.chips.flex_cluster.flex_cluster \
  --binary /Users/jerryyun/projects/SoCDAML-native/logs/presentation-grid-broadcast-v2-71uk1e55/grid_broadcast.elf \
  run --trace-level=debug \
  '--trace=cluster_[0-9][0-9]*/pe0/spatz/trace$' \
  '--trace=cluster_[0-9][0-9]*/idma/fe/trace$'
```

Fresh evidence directory:
`SoCDAML-native/logs/presentation-grid-broadcast-v2-71uk1e55/`, with adjacent
`.tar.gz`. It includes preflight/source/build/checker provenance, ELF/dump,
raw trace, runtime configuration, progress, host stack sample, wrapper source
excerpt, model-config comparison and `failure-review.json`. Final archive
members and accepted direct artifacts were hash-verified after the run.

### Outcome and last confirmed progress

`report.json`: `status: failed`, `timing_accepted: false`,
`stop_reason: time_limit`, one simulation, intentional exit -9 after
**300.014 host seconds**, with **35,613 captured bytes**. The log was the same
size at every 30-second checkpoint. Host runtime is not simulated performance.

Model construction/reset deassertion and the SystemC startup banner are the last
confirmed trace progress. There are **no recorded BEGIN/INTERVAL_END/END markers,
DMA completions, vector completions, TIMES/COMPUTE records or validation words**.
This event-only trace does not establish whether any target instructions ran,
nor locate an exact stalled PC. It does not reach a broadcast-transfer event.

One concurrent one-second sample of this same launcher (PID 62503, approximately
81 seconds after launch) was retained as `launcher-sample.txt`. SystemC/DRAMSys
controller/refresh processing was active; the GVSoC wrapper was suspended in
`my_module::run`, `main_systemc.cpp:60`, the `wait(sync_event)` path. No ISS
execution was sampled. This differs from the prior sandbox locale-parsing
slowdown and is not a sampled blocked stdout write. It suggests an issue to
investigate in startup/event scheduling, but **does not establish the cause**.

The generated runtime config has no hardware/clock differences from direct
after normalizing the variant ELF path and 96 derived `debug_binaries` sidecar
names. `runtime-config-differences.json` and `runtime-model-comparison.json`
retain that comparison. The harness's generic post-run equality guard would
also need to normalize these derived debug basenames; it was not reached,
because the time-limit check failed first. No model setting or kernel fix was
made or inferred from those metadata filenames.

### Acceptance and comparison withheld

- Validation: **0 / 262,144** words recorded; correctness unverified.
- Vector workload: **0 / 82,944** completions recorded; all-worker execution
  and computation completion bounds unverified.
- Measured interval and native exclusion of validation-only synchronization:
  not reached/verified. Matching source definitions alone do not accept timing.
- Kernel, installed model/dependencies, original/profile source and all prior
  prepared/direct artifact preservation checks passed before and after.

**No broadcast phase durations, time saved or speedups are accepted or
calculated.** The accepted direct figures remain 13.565 µs complete input stage,
13.259 µs actual DMA envelope (including schedule gaps), and 63.720 µs continuous
application. They cannot be compared with this failed capture.

For any later successful broadcast run, retain separate HBM schedule (t1−t0),
leader/input join (t2−t1), distribution (t3−t2), distribution completion join
(t4−t3), complete input-ready stage (t4−t0), compute/join (t5−t4), output schedule
(t6−t5), final join (t7−t6), and continuous interval (t7−t0). Its actual DATA-IN
envelope must run from the first HBM DMA start through the **last BA/BB
distribution completion**, including delivery to consuming clusters—not only
leader HBM fetching. Do not sum overlapping per-cluster durations.

### Next single action

Inspect the retained startup/GVSoC–SystemC event-handshake evidence and plan a
focused startup instruction/clock trace before requesting another simulation.
Do not rerun this unchanged command or extend limits. This one-run broadcast
authorization is exhausted; accepted direct evidence remains preserved.

## 2026-10-26 — broadcast startup forensic review; diagnostic prepared, NOT RUN

Read-only comparison is documented in [BROADCAST_STARTUP_REVIEW.md](BROADCAST_STARTUP_REVIEW.md).
Both evidence directories/archives remain unchanged; their artifact, report and
archive identities are recorded under native `logs/broadcast-v2-startup-review-am8906um/`.
No simulation, rebuild, completed test repetition or behavioral/model change ran.

- Both captures selected only all-cluster pe0 Spatz and DMA-front-end traces at
  debug level. Instruction, exception, barrier, loader and fetch/clock traces
  were absent. stdout/stderr were merged into a regular file, with no explicit
  unbuffering; SIGKILL could lose a buffered tail. BEGIN follows barrier setup,
  576 initialization transfers and vector setup. **No BEGIN does not mean main
  never executed.** Missing later markers cannot locate an earlier stop.
- Actual argv differs only in ELF path; cwd, controlled environment setup,
  output handling, source/model/dependency hashes and hardware/clock config
  match. Derived debug-sidecar names differ. Original inherited environment
  values were not archived, nor was an accepted direct-V2 loaded-image map.
  Broadcast's SystemC/DRAMSys/libpulpvp UUIDs match the older successful direct
  replay sample; exact accepted direct-V2 library loading is not reconstructable.
- Both ELF entries are 0x80000000 and PT_LOAD mappings match, ending at
  0x803f0104 inside instruction memory. No overlap defect found. Startup source
  and stack setup match; gp, main address and code/data layout legitimately
  differ after compilation. The BEGIN argument is the first semantic variant
  indicator; the actual strategy-specific HBM schedule follows BEGIN/t0.
  There is no evidence that broadcast-specific fetch/distribution was reached.
- `main_systemc.cpp:60` normally waits when next_timestamp==-1 and retain==1.
  A new head-of-queue event calls TimeEngine::enqueue → was_updated →
  sync_event.notify. A pending wakeup/notification chronology was not captured.
  The single DRAMSys-active/wrapper-wait sample establishes sampled quiescence,
  not deadlock, a lost notification, or an exact target PC.

Ranked possibilities: (1) startup/event liveness failure leaves GVSoC without
future work/wakeup; (2) a target blocks in common pre-BEGIN FP/barrier/initial-DMA
code and the wrapper legitimately waits for its missing response. Neither is
proved. Limited output buffering remains an observability caveat, not a credible
sole explanation for hiding an entire normal DMA/vector run. Prior direct timeout
and successful unchanged direct captures weaken a broadcast-only kernel theory.

Prepared diagnostic: native `logs/diagnose_broadcast_v2_startup.py`, AST-checked
only. Proposed command, **not executed and requiring new simulation authorization**:

```bash
cd /Users/jerryyun/projects/SoCDAML-native
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/diagnose_broadcast_v2_startup.py
```

It reuses the unchanged broadcast ELF/model, creates fresh evidence, drains a
merged PTY, and stops at **60 seconds / 64 MiB**, without retry or timing
acceptance. Traces: cluster-0 pe[012]/insn; all-cluster pe[012] wrapper/exec
(fetch/clock), exception, cluster_registers and DMA-front-end; cluster-0 loader;
chip control registers. Concurrent one-second samples at 2/20/45 seconds retain
library images, subject to the capture ending earlier. A small relevant environment
allowlist is recorded. Instruction trace may hit the cap during normal init;
that proves observed progress, not the old stall. PTY/tracing perturbs scheduling
and cannot retrospectively establish the original cause. Next action is review
and authorize this focused diagnostic, not an unchanged 300-second rerun.

## 2026-10-26 — one authorized startup diagnostic: initialization progressed; output cap

Ran exactly one diagnostic with **60 seconds / 64 MiB**, no retry or rebuild.
Command from native workspace, outside the sandbox:

```bash
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/diagnose_broadcast_v2_startup.py
```

Fresh evidence: `SoCDAML-native/logs/broadcast-v2-startup-diagnose-6i2dfqaz/`
and adjacent `.tar.gz`. `report.json` archives the exact simulator argv, cwd and
relevant non-secret environment; `regex-verification.json` records actual libc
regcomp/regexec checks using flags=0, matching the pinned engine's POSIX-basic
syntax. All 16 loader/barrier/DMA/Spatz paths, 48 wrapper/exec/exception paths,
and chip-control path matched. stdout/stderr were continuously drained from a
PTY. Original direct/failed-broadcast artifacts and archives, broadcast ELF,
installed model, dependencies and source manifests verify preserved.

Removed `/insn` and added all-cluster loader/Spatz completion filters before
launch. **I missed another continuous instruction source:** the retained `/exec`
channel emits `Executing instruction` through
`core/models/cpu/iss/include/exec/exec_inorder_implem.hpp:199`, at the default
message level. Correct path-regex coverage did not establish event-only output.
This invalidates the earlier assertion that debug-level exec tracing is limited
to fetch/clock control. No benchmark/model behavior was changed to address it.

The capture stopped at **67,108,864 bytes after 6.548 host seconds**, with
491,605 per-instruction exec messages. No limit extension or follow-up run occurred.
The sample scheduled at 2 seconds was collected concurrently while the process
was alive; the capture ended before 20 and 45 seconds, so those samples were
not taken. This is an output-limit diagnostic, not a benchmark pass.

Confirmed progress (`analysis.json` retains counts and trace locations):

- All **16 loaders** processed 64 section chunks each (1,024 total), through
  0x803f0000 / final 0x104-byte chunk (`trace.log:1881`). All **48 cores** received
  boot address 0x80000000 and fetch-enable=1 (`trace.log:1882–1929`), and execution
  records are present for all 48. DMA reset deassertions are recorded for all
  16 clusters. No `Setting clock` text event appeared, but progressing target
  execution/cycle timestamps prove clock activity in this capture.
- All **16 clusters** recorded the initial Type-0 synchronization annotation
  (`trace.log:7865–7966`). Cluster-0 DM proceeded through main into HBM
  initialization: **11 A-row transfers, rows 0–10**, completed. No B/C-init row
  transfer completed yet. Last completed DMA is transaction 10, destination
  0xc0000e00, at simulated 184,427 ns (`trace.log:491555`).
- Last complete cluster-0 DM dispatch record is pc=0x80000140 in `__clzsi2`,
  used in A-value bit construction (`trace.log:493561`); this is not a retired-PC
  guarantee or proof of the blocked instruction. Pe0/pe1 last appear at
  0x800026e4, the next global-barrier entry, consistent with waiting while DM
  fills HBM. Initialization/barrier progress is confirmed, not a measured-input
  or broadcast-distribution result. No BEGIN, GEMM completion or validation
  marker was reached; no exception event was recorded.
- `sample-2s.txt:97–110` shows active step_until → TimeEngine/ClockEngine → ISS,
  plus substantial trace vfprintf/write activity. Timed SystemC waits are also
  present. It does not show the prior persistent wait at main_systemc.cpp:60
  during the sampled interval. Heavy trace-I/O overhead/output pressure is
  supported; an undrained collector or persistent host-write deadlock is not.
  Only one sample exists, so no persistence conclusion can be drawn.

This launch demonstrates normal early target progress and diagnostic-induced
output-budget exhaustion. It **does not reproduce/explain the original timeout,
prove a fix, complete the application, or accept any performance result**.

Next single action: prepare and review **control-only fetch/clock observation
without the text `/exec` channel**, checking suitable registered change events
offline before requesting another capture. No additional simulation is authorized
or launched here. Do not repeat the current noisy diagnostic unchanged.

## 2026-10-26 — corrected event-only broadcast-V2 verification PASSED

Used exactly one authorized **300-second / 64-MiB** simulation. No retry,
rebuild, direct rerun or repeated completed tests. Prepared broadcast ELF,
installed model and earlier evidence are verified unchanged. All five checks
passed: **262,144 exact words**, bit-identical delivered inputs/final HBM outputs
versus direct, **82,944 expected vector completions** across all 16 clusters,
624 DMA completions within issue/wait and phase bounds, validation/readbacks
and validation-only synchronization after t7, plus workload/kernel/model/source
preservation. Every worker's last vector completion precedes its end probe by
five cycles. Kernel SHA-256 remains
`c8a8a2f32342ef6cb19921a5fb3f25cb8de9467b283562d2a54be7e086c7e3d6`.

From native workspace, outside the sandbox:

```bash
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/verify_grid_broadcast_v2_pty.py
```

Evidence: native `logs/presentation-grid-broadcast-v2-pty-69p_kpqx/` and adjacent
`.tar.gz`; all **33 artifact hashes** and archive members verify. `report.json`
contains exact child argv/cwd, non-secret environment and sample outcomes;
`verification.json` and `comparison.json` contain acceptance and timing details.
The harness reuses the previous verifier with continuously drained PTY capture;
config comparison normalizes only ELF/debug filenames and trace selectors,
leaving hardware, timing, ISA and topology checked.

Final DEBUG allowlist is anchored to `/chip/cluster_[0-9][0-9]*`: loader/trace,
pe[012]/exception, cluster_registers/trace, idma/fe/trace, pe0/spatz/trace.
**No exec, insn, decoder, wrapper, fetch/clock or memory-access selection.**
Call-site inspection confirms bounded loader chunks, exceptions, sync transitions,
DMA and vector completions; polling prints are commented and low-level DMA
messages require TRACE=4 above DEBUG=3. Archived offline libc regcomp/regexec
(flags=0, pinned POSIX-basic syntax) verifies coverage and forbidden-path rejection.
No Executing-instruction or forbidden trace records occur in the capture.

Normal exit after **138.696 host seconds**, **19,065,458 captured bytes**; host
runtime is not performance. Samples submitted concurrently at 2/20/45 seconds:
launcher not yet found at 2s; 20s captured; 45s helper timed out after its 5s
bound. Output draining continued and failed samples were not retried.
BEGIN/INTERVAL_END/END are trace lines 2344/85896/87913.

At **1 GHz**, cycles equal simulated ns:

| Adjacent broadcast phase | µs |
|---|---:|
| HBM input schedule | 3.175 |
| HBM/leader completion join | 0.411 |
| Broadcast distribution schedule | 2.935 |
| Distribution completion join | 0.383 |
| Compute + required synchronization | 42.748 |
| Parallel DATA-OUT master progress | 0.680 |
| Final DATA-OUT completion/synchronization | 2.941 |
| **Continuous application** | **53.273** |

Complete input readiness is **6.904 µs**, including HBM fetching, distribution
and **0.794 µs** of explicit joins. Joins include remote DMA completion tails,
not pure barrier overhead: last HBM completion is 318 ns after t1, and last
distribution completion is 279 ns after t3. Readiness follows the final input
completion by 104 ns. Scheduling barriers remain inside schedule phases.
Actual DATA-IN engine envelope is **6.682 µs**, first HBM start through last
BA/BB completion, including gaps; software issue/wait envelope is separately
6.699 µs. No overlapping cluster durations are summed. Every compute call is
42.654 µs; compute-join waits are 85–93 ns (worker 0: 89 ns), already included.
Output through final synchronization is **3.621 µs**, not only 0.680 µs.

| Identical-boundary comparison | Direct µs | Broadcast µs | Direct/broadcast | Saved µs |
|---|---:|---:|---:|---:|
| Complete input readiness | 13.565 | 6.904 | 1.964803× | 6.661 |
| Actual DATA-IN envelope, including distribution | 13.259 | 6.682 | 1.984286× | 6.577 |
| Output through final completion | 7.410 | 3.621 | 2.046396× | 3.789 |
| **Continuous application** | **63.720** | **53.273** | **1.196103×** | **10.447** |

Input time falls 49.104%; application time falls **16.395%**. Saved application
is 6.661 µs input + 3.789 µs output − 0.003 µs compute-phase increase.
**Direct output is serialized; broadcast output is parallel**, confirmed by
actual DMA overlap checks. Overall speedup compares these full schedules;
it must not be attributed solely to input broadcasting.

One accepted sample per strategy, no variability estimate or exact historical
slide equivalence. This successful PTY/event capture **does not establish the
cause or resolution of the earlier timeout**; host observation changes may
perturb scheduling. No runtime fix was made.

Next action: consolidate the accepted pair's commands, provenance and measurement
definitions into the reproduction results README. No further simulation is
authorized or needed for this verification step.

## 2026-10-26 — repository consolidation completed; zero new simulations

The accepted pair is now packaged in the cleaned repository. See
[SPATZ_GRID_V2_RESULTS.md](SPATZ_GRID_V2_RESULTS.md) and
[`tools/socdaml/spatz_grid_v2/README.md`](../../tools/socdaml/spatz_grid_v2/README.md).
Root and GEMM READMEs link the current reconstruction separately from the
preserved recovered K=256 scalar sources.

- Exact accepted `grid.c`, export header, shared `gemm_fp32.S` and linker script
  are under `soft_hier/flex_cluster_sdk/app_example/gemm/spatz_grid_v2/`, with
  attribution intact. The original benchmarks 4/5 and existing legacy sources
  were not edited. Exact accepted parser/helper modules and historical runner
  snapshots are retained in the tool package; accepted helper function bodies
  were extracted unchanged, with repository-owned imports/globals.
- `reproduce.py` provides check/build/run/validate/results commands independent
  of Downloads kits or code stored under logs. Only explicit `run` simulates,
  once, fixed at 300 seconds / 64 MiB with drained PTY and concurrent samples.
  `build` uses the existing compiler and never rebuilds/patches the model.
  Reference source/model/dependency/compiler manifests and revisions are local
  to the package. Old log-only scripts remain untouched as evidence.
- Full measurement definitions, validation-only barrier exclusions, supported
  DMA-envelope versus complete-readiness boundaries, phase/comparison tables,
  complete-schedule attribution, revisions/compatibility diffs and report/archive
  hashes are documented. Machine-readable `accepted-results.json` and generated
  PNG/SVG phase charts live in `docs/project/reproduction/spatz_grid_v2/`.

Checks completed, without simulation or environment rebuild:

1. Verified all 24 direct and 33 broadcast artifact hashes and corresponding
   archive members, and rechecked original report/archive identities after work.
2. Packaged dependency/path/model/compiler checks pass. New CLI revalidates both
   raw captures from outside the repository working directory: all 262,144 words
   and 82,944 vector operations per variant, raw DMA/completion boundaries,
   matching inputs/outputs and model fingerprint. Accepted metrics unchanged:
   input 13.565/6.904 µs; application 63.720/53.273 µs; 1.196103× overall,
   10.447 µs saved. Direct output serialized; broadcast output parallel.
3. Three focused packaging checks pass: exact source/original hashes, unchanged
   accepted helper AST bodies, and config guard sensitivity to clock/latency.
   Previously completed 48-thread/native simulation checks were not repeated.
4. Compile-only packaging build `SoCDAML-native/logs/spatz-grid-v2-build-qcdpszj6`
   succeeds for both variants. All allocated sections (.text, .rodata, .sdata,
   shared GEMM), including addresses, equal their corresponding accepted ELF
   byte-for-byte. This strengthens the no-timed-execution-change check.
5. Generated chart from integer simulated-ns JSON, verified phase sums and
   visually inspected PNG. Checked 24 documentation links and diff whitespace.
   Receipt: `docs/project/reproduction/spatz_grid_v2/packaging-checks.json`.

No commit, push, environment rebuild, benchmark algorithm/synchronization change
or unrelated-project access occurred; pre-existing dirty files/submodules remain.
Each variant still has exactly **one accepted simulation**, and the earlier
intermittent timeout remains unexplained. The successful capture is not a
historical-slide equivalence or a demonstrated startup fix.

Remaining packaging limits: newly packaged orchestration has not been simulated
(explicitly excluded here); it reuses accepted capture/check helper bodies and
produces identical executable content. Existing pinned native profile/toolchain
and external raw captures are required. Fresh-machine provisioning/arbitrary
workspace relocation are not certified; no installer is added. Metadata and
small provenance snapshots are committed-ready locally, but no Git commit was made.

Next action: review the repository-owned results/workflow. No additional
simulation is required for consolidation; repeatability work would be a separate,
explicitly authorized experiment.

## 2026-10-26 — packaged CLI end-to-end checkpoint verified

Used exactly the two newly authorized simulations: direct first, then broadcast,
each fixed at **300 seconds / 64 MiB**, no retries. Both passed all numerical,
workload, raw DMA/vector timing-boundary and preservation checks. No model/
environment rebuild or new simulator investigation occurred.

From native workspace, outside the sandbox, using the existing packaging build:

```bash
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python /Users/jerryyun/projects/SoCDAML-Project-cleanup/tools/socdaml/spatz_grid_v2/reproduce.py --native-root /Users/jerryyun/projects/SoCDAML-native run --variant direct --elf /Users/jerryyun/projects/SoCDAML-native/logs/spatz-grid-v2-build-qcdpszj6/grid_direct.elf
PYTHONDONTWRITEBYTECODE=1 venv/bin/python /Users/jerryyun/projects/SoCDAML-Project-cleanup/tools/socdaml/spatz_grid_v2/reproduce.py --native-root /Users/jerryyun/projects/SoCDAML-native run --variant broadcast --elf /Users/jerryyun/projects/SoCDAML-native/logs/spatz-grid-v2-build-qcdpszj6/grid_broadcast.elf
```

Fresh native evidence and adjacent archives:

- `logs/spatz-grid-v2-direct-63hpp47d`: normal exit, 111.088 host seconds,
  19,189,171 captured bytes; all 262,144 words and 82,944 vector operations checked.
- `logs/spatz-grid-v2-broadcast-1ykiy6_p`: normal exit, 106.424 host seconds,
  19,065,279 captured bytes; all 262,144 words and 82,944 vector operations checked.

All adjacent phases, complete-input/application intervals, input/output envelopes,
per-worker compute calls and synchronization waits match their respective original
accepted capture exactly: **all differences 0 cycles/ns**. Input remains
13.565 / 6.904 µs; application remains 63.720 / 53.273 µs. No discrepancy was
averaged away. Original reports, archives, `accepted-results.json` and chart are
preserved unchanged. Supplemental evidence identities, artifact/archive hashes,
phase values/deltas and commands are recorded separately in
`docs/project/reproduction/spatz_grid_v2/packaged-workflow-verification.json`.
Both new archives' artifact members were verified.

The repository workflow has now completed end to end. There are two accepted
observations per variant including the originals, a small repeatability check,
not a statistical stability guarantee. **Intermittent startup timeout remains
unexplained and is not claimed fixed. Fresh-machine provisioning/relocation
remains unverified.** Host capture duration is not simulated performance.

Local checkpoint message: `Package validated Spatz grid reproduction and results`.
Checkpoint scope is repository-owned V2 code, source/provenance/results/chart,
workflow documentation and handoff, plus only the V2 portions of existing
README diffs. Earlier proposal/scalar edits, dirty submodules, native builds,
toolchains and raw traces are excluded. No push or amend is authorized.
