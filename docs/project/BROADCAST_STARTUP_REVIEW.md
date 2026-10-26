# Broadcast-V2 startup review — 2026-10-26

**Subsequent diagnostic update:** the authorized capture
`broadcast-v2-startup-diagnose-6i2dfqaz` progressed through loading, fetch enable,
initial synchronization and 11 A initialization rows, then hit 64 MiB. Removing
`/insn` was insufficient: `/exec` also emits per-instruction text at its default
message level. The earlier assumption below about low-volume debug exec tracing
was wrong. See CODEX_STATUS.md for the executed command, evidence and limitations;
do not repeat the noisy diagnostic unchanged.

Read-only investigation of accepted direct `presentation-grid-direct-v2-0puwdfmc`
and failed broadcast `presentation-grid-broadcast-v2-71uk1e55`. Both evidence
directories and archives remain unchanged. No simulation, build, model patch or
completed test was run. A diagnostic harness was prepared and syntax-checked only.

All paths below are relative to `/Users/jerryyun/projects/SoCDAML-native` unless
otherwise stated. Pinned source prefix: `profiles/presentation-spatz-v1/source/`.

## Confirmed observations and observability

Both captures enabled precisely these POSIX-basic regex filters at debug level:

```text
cluster_[0-9][0-9]*/pe0/spatz/trace$
cluster_[0-9][0-9]*/idma/fe/trace$
```

The native trace engine uses `regcomp(..., 0)` (`core/engine/src/trace/trace_domain_impl.cpp:352`).
They match all 16 intended clusters, as demonstrated by the accepted direct run.
The config's `traces.enabled=false` does not disable the explicit include list.
Instruction/decode, ISS wrapper/exec, exception, loader, barrier and clock-control
traces were **not** selected. Thus none of those missing events can localize this
failure. In particular, no last retired instruction or blocked PC was captured.

Both harnesses used `subprocess.Popen` with `stdin=DEVNULL`, stdout to a newly
opened regular binary file, stderr merged into stdout, a fresh process group,
the same 300-second wall deadline and hard 64-MiB `RLIMIT_FSIZE`. No PTY, explicit
unbuffering, or target-output flushing was requested. At timeout the broadcast
process group was killed with SIGKILL, so an unflushed tail can be lost.
Trace records use stdout by default (`trace_domain_impl.cpp:149`) and `vfprintf`
(`core/engine/src/trace/trace.cpp:141`). Target `_putchar` writes MMIO 0x90000010;
`soft_hier/flex_cluster/ctrl_registers.cpp:201–205` sends each character to
`std::cout`, without an explicit per-character flush. Target prints bypass trace
selection but are subject to host output buffering.

| Missing item in broadcast | Should it have been observable? |
|---|---|
| BEGIN | Yes if cluster-0 DM reaches and emits it **and output flushes**. It occurs well after entry/main and initialization, so absence does not mean main never ran. |
| Initialization DMA completions | Enabled on every engine. Cluster 0 waits after each of 576 transfers; a completed initialization should generate many records. A buffered tail could hide a few early events, not plausibly the whole normal initialization plus completed application. A request still in flight produces no Finished record. |
| Vector completions | Enabled on every pe0; expected 82,944 records only after the input stages. Startup `fcvt.d.w` and vector configuration are not this GEMM completion count. Absence does not show where earlier execution stopped. |
| INTERVAL_END | Unfiltered target print, after t7 and a validation-only barrier. Would appear upon reaching/flushing that point; neither happened observably. |
| TIMES/COMPUTE/DMA span records, WORDS64, END | Unfiltered target prints, all after INTERVAL_END. Their absence is consistent with any earlier stop and gives no additional startup location. |

Broadcast retained only model construction/reset messages and SystemC banners:
35,613 bytes, unchanged at every 30-second checkpoint. Last selected reset
deassertion is `trace.log:621`, final log line 628. No BEGIN, DMA Finished,
Spatz Finished, application timestamps or validation frame was recorded.

## Launch conditions and evidence limits

Both actual commands use the same installed `gvsoc`, target, debug level and two
filters above. The sole simulator argv difference is the binary path/name:

- direct: `logs/presentation-grid-direct-v2-0puwdfmc/grid_direct.elf`
- broadcast: `logs/presentation-grid-broadcast-v2-71uk1e55/grid_broadcast.elf`

Both child working directories are `profiles/presentation-spatz-v1/source`.
Both were launched outside the sandbox after `source env.sh`, using the same
venv Python and `PYTHONDONTWRITEBYTECODE=1`. The archived `profile_env` function
(`checking-tools/presentation_spatz.py:137–152`) inherits `os.environ`, removes
native/source PATH entries, prepends profile tools and source/gapy, sets profile
PYTHONPATH and disables bytecode writes, and removes SOCDAML_APP. The installed
bash launcher also prepends its install/bin and install/lib to PATH and
LD_LIBRARY_PATH. It does not sanitize DYLD variables or locale/timezone settings.

**Neither original run archived an environment snapshot.** Exact inherited
LANG/LC_*/TZ, DYLD_* and other relevant overrides cannot now be compared
retrospectively. Matching setup commands are not proof that all inherited values
matched. The proposed diagnostic records a small relevant allowlist.

Archived installed-artifact, source and SystemC/DRAMSys dependency manifests
match and verified before/after. Config comparison is identical after normalizing
ELF paths and 96 derived `debug_binaries` sidecar filenames; those are metadata
differences, not hardware/clock changes. The broadcast harness's post-run config
guard did not normalize sidecar basenames, but was never reached: timeout was
the earlier rejection. It is not the simulator's stopping cause.

No runtime loaded-image inventory was collected for the accepted direct-V2 run.
Broadcast's sample identifies libpulpvp-debug, SystemC 2.3.3 and DRAMSys. Their
Mach-O UUIDs match the **older successful direct replay** sample exactly:

| Library | UUID in both available samples |
|---|---|
| libpulpvp-debug | 4CC1216A-1423-3D2E-A3B9-A77F7B6DFEB2 |
| SystemC | 2B7B19DB-7EBB-3136-958D-D26853FE7BE4 |
| DRAMSys | A7FC705D-F69B-3E53-97FD-B20A55B847D1 |

`otool -L` confirms the pinned launcher links the workspace SystemC dylib and
`@rpath/libpulpvp-debug.dylib`. This supports library continuity, but does not
reconstruct the accepted direct-V2 process's complete actual library map.

## Executable startup and first strategy-specific operation

Both are ELF32 little-endian RISC-V executables, flags 0x4 (double-float ABI),
entry **0x80000000**, with identical program-header mappings. The substantive
PT_LOAD is offset 0x1000, virtual=physical=0x80000000, file=memory size 0x3f0104,
read/execute, alignment 0x1000. The second PT_LOAD is empty in both. Thus the
mapped executable ends at 0x803f0104, within the 4-MiB instruction region.

| Section | Direct | Broadcast |
|---|---|---|
| .text | 0x80000000, size 0x31c4 | 0x80000000, size 0x339c |
| .rodata | 0x800031c8, size 0x568 | 0x800033a0, size 0x570 |
| .sdata | 0x80003730, size 0x10 | 0x80003910, size 0x10 |
| shared GEMM | 0x803f0000, size 0x104 | identical placement and bytes |

No section overlap or loader-placement defect was found. The startup assembly
source is the same (`soft_hier/flex_cluster_sdk/runtime/flex_start.s`): zero
integer registers, inspect misa and clear FP registers using fcvt.d.w, set gp,
read mhartid, set stack, call main. Linked immediates legitimately differ:
gp=0x80003f20 versus 0x80004100 and main=0x800027fc versus 0x8000285c. The stack
region is identical, 0x10000000 with size 0x20000; initial SP is
0x10020000−(hartid<<10). Neither executable has an allocated BSS section requiring
a distinct zeroing path. Layout differences alone do not demonstrate a fault.

Main's common pre-BEGIN path is `grid.c:155–167`: barrier initialization, first
global barrier, local NaN poisoning, cluster-0 HBM A/B/C initialization (576
DMA-and-wait operations), core-0 vsetvli/vtype setup and another global barrier.
Barrier helpers contain potentially blocking CSR reads of 0x7c2; startup FP
offload, barrier waits, initialization and the first outstanding DMA are all
unobserved by the original trace selection.

There is no runtime `if (GRID_BROADCAST)` selector: the compile-time flag folds
the branch. The earliest semantic variant indicator before measurement is the
BEGIN argument `broadcast=1` (broadcast disassembly 0x800031f8 / printf call
0x80003204). It is just a print argument, not a distribution operation. The
first strategy-specific source path is **after BEGIN and t0**, `grid.c:178–195`:
row leaders fetch A, then column leaders fetch B instead of direct's 16-worker
serial input schedule. For cluster-0 DM, t0 is at 0x80003210, the initial schedule
barrier call at 0x8000322c, row test at 0x80003230 and first A DMA issue at
0x800032b0–0x800032b8. Actual broadcast distribution calls are later (e.g.
0x800032f0 → broadcast at 0x80000f30). There is **no execution evidence** that
any of these broadcast-specific operations was reached. Earlier register/code
layout differences must not be mistaken for evidence of a failed broadcast DMA.

## What releases the sampled wait

Pinned `core/engine/src/main_systemc.cpp:43–72` calls step_until at current
SystemC time. With retain_count==1 and returned next_timestamp==-1 it performs
`wait(sync_event)` at line 60. A finite next timestamp uses the timed-or-event
wait at line 64 instead. `my_module::was_updated()` calls `sync_event.notify()`.

The callback is bound by `GvsocLauncher::bind` (`launcher.cpp:112–115`). In
`core/engine/src/time/time_engine.cpp:301–337`, inserting a client at the head
of the event queue calls the bound launcher's `was_updated()`. Queue-empty
get_next_event_time returns -1 (lines 141–148). CPU clock/fetch activation,
loader start, or an asynchronous device response that schedules GVSoC work
should ultimately arrange an event/notification through this machinery. Loader
completion publishes entry/start (`core/models/utils/loader/loader.cpp:204–219`);
CPU exec traces can expose fetch, boot-address and clock activation.

The wait is **normal for an empty GVSoC event queue**, including cores waiting
for a barrier/device event. A stalled handshake would require evidence that
work/a wakeup was due or newly queued but the wrapper remained asleep. Neither
queue contents, notification chronology nor pending response state was captured.
The one-second sample around 81 seconds shows DRAMSys controller/refresh work
and the wrapper asleep, with no ISS execution sampled. It proves sampled
quiescence, not deadlock, lost notification, or absence of earlier execution.

## Ranked explanations — two, neither confirmed

1. **Startup/event liveness failure:** GVSoC becomes quiescent and expected
   startup/clock/device activity does not reawaken it. This best fits the wait
   sample and flat event log, and may include an unscheduled event or missed
   notification. No specific race is proved. The old direct timeout with a
   similar reset-only tail and later successful unchanged direct captures weakens
   a broadcast-only kernel explanation and makes launch/event liveness worth
   testing first.
2. **Target reaches a common pre-BEGIN blocking operation:** FP startup,
   initial barrier, or initial DMA stalls and GVSoC then legitimately waits for
   a response/acknowledgment. The trace lacks exactly the instruction/barrier/
   exception events needed to distinguish this from explanation 1. Code layout
   differences may affect execution, but there is no evidence identifying one
   as defective.

Output buffering is an observability limitation, not a persuasive sole explanation
for hiding a normal full run with 576 initialization DMA records and 82,944 vector
records. Unrecorded inherited environment differences remain a confound, not an
evidence-supported third cause. The old sandbox locale-parsing sample is a
different host stage from this run and should not be conflated with it.

## Prepared diagnostic — do not execute without new authorization

`logs/diagnose_broadcast_v2_startup.py` is prepared and AST syntax-checked only.
Exact proposed command, from the native workspace in a normal Terminal/outside
the sandbox:

```bash
cd /Users/jerryyun/projects/SoCDAML-native
source env.sh
PYTHONDONTWRITEBYTECODE=1 venv/bin/python logs/diagnose_broadcast_v2_startup.py
```

The harness copies the unchanged failed broadcast ELF into fresh evidence,
verifies both prior directories/archives and model/source/dependencies before
and after, and runs at most **60 seconds or 64 MiB of captured simulator output**,
whichever comes first. It does not rebuild, change timing/synchronization or
retry. stdout/stderr go to one continuously drained PTY to expose newline output.
It always rejects diagnostic timing. Trace level remains debug, with:

```text
cluster_0/pe[012]/insn$
cluster_[0-9][0-9]*/pe[012]/wrapper$
cluster_[0-9][0-9]*/pe[012]/exec$
cluster_[0-9][0-9]*/pe[012]/exception$
cluster_0/loader/trace$
cluster_[0-9][0-9]*/cluster_registers/trace$
cluster_[0-9][0-9]*/idma/fe/trace$
/chip/ctrl_registers/trace$
```

Cluster-0 instructions identify entry, main, initialization and CSR/DMA polling;
all-cluster exec/wrapper events identify reset, fetch and clock controls; loader,
barrier and DMA events identify pending startup dependencies. **Correction from
the subsequent run:** exec traces emit both clock/fetch state and a default-level
`Executing instruction` message for each instruction. Debug level does not avoid
that chatter; a revised plan must omit text `/exec` and validate another way to
observe control changes.
Concurrent one-second launcher samples are scheduled at 2, 20 and 45 host seconds
while output keeps draining; Binary Images records capture loaded-library UUIDs.
A relevant environment allowlist is archived without dumping the whole environment.

Instruction logging may reach 64 MiB during otherwise normal initialization, as
the older direct diagnostic did. That would establish target progress through
the observed last instruction, not reproduce this no-progress failure. Samples
scheduled after an early limit are intentionally skipped. A last retired PC may
precede a blocked instruction; decoder absence and a single wait sample never
prove the blocked PC. If an event/notification race remains after this capture,
queue/notification instrumentation would be a separately proposed next step,
not a justified model fix or automatic additional run.

PTY and extra tracing perturb host scheduling. A progressing diagnostic therefore
does not prove the original stalled condition was fixed or retrospectively caused
by buffering. No such diagnostic has been executed in this review.
