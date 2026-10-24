# GEMM benchmark sources

These are cleaned derivatives of Jerry Yun's `benchmark_1.c` through
`benchmark_5.c` at superproject revision
`707cc6f0db370d16f608de7b9ff86e9c758d95e9`.
The five algorithms remain separate files so their schedules can be reviewed
against the presentation. Their main functions were not replaced with the
common schedule used by the earlier matched row experiment.

## Workload and ownership

All matrices are row-major FP32. `M` is the number of output rows, `K` is the
reduction dimension, and `N` is the number of output columns. Each worker owns
one 64×64 output tile; x selects output columns and y selects output rows.

| Source | Current M,K,N | Worker set | Presentation intent |
| --- | --- | --- | --- |
| `single_cluster.c` | 64,256,64 | C[0,0] DM | Single-cluster cost, slide 8 |
| `row_direct.c` | 64,256,256 | y=0, x=0..3 DM | Repeated A reads, slide 10 |
| `row_broadcast.c` | 64,256,256 | y=0, x=0..3 DM | Reuse A across one row, slide 12 |
| `grid_direct.c` | 256,256,256 | All 16 DM cores | Independent A/B reads, slide 14 |
| `grid_broadcast.c` | 256,256,256 | All 16 DM cores | Share A by row and B by column, slides 14–15 |

These dimensions describe the recovered sources. **The slides use K=64.**
All five cleaned files call `gemm_scalar_fp32`; the renamed function preserves
the original arithmetic loop and makes the implementation explicit. There is
no recovered Spatz kernel in these five programs.

## Data movement

A row strip is contiguous, so it uses a 1D DMA. A B column tile is strided in
the full HBM matrix: a 2D gather copies K rows of 64 FP32 elements into packed L1
storage. A C tile uses a 2D scatter with the full output width as its HBM stride.
For the single-cluster case, all three matrices are contiguous.

The row direct case fetches A four times; its broadcast partner fetches A once
and still fetches four B tiles. The grid direct case fetches each A row strip
four times and each B column strip four times; its broadcast partner fetches
each unique strip once. These transfer counts explain the intended reuse.
They do not predict a speedup by themselves.

Broadcast destinations require equal L1 buffer offsets. Both row variants now
free their completed preload allocations before allocating worker buffers.
Grid variants allocate worker A/B/C buffers before allocating the master's
initialization scratch row, preserving equal worker offsets.

The pinned NoC router uses **coordinate-match masks**:

`(source_x & row_mask) == (destination_x & row_mask)` and
`(source_y & col_mask) == (destination_y & col_mask)`.

Consequently a row broadcast uses `(0x0, 0xf)` and a column broadcast uses
`(0xf, 0x0)`. These are not recipient bitmaps. The SDK broadcast wrapper takes
L1 offsets and constructs complete local/remote DMA addresses.

## Scheduling and timing

- Global barriers involve the entire 4×4 grid, including inactive cores.
- The original row programs serialize input DMA slots and output DMA slots.
  They also synchronize before compute; this differs from slide 11's notes
  about starting compute when each worker's input arrives.
- Grid direct serializes worker loads and stores. Grid broadcast serializes
  leader input/broadcast steps, then stores output tiles concurrently.
- Only the C[0,0] DM core reads the global timer. Each timer call is now
  annotated with a stable interval name in a comment, without extra prints
  or barriers.
- HBM initialization is outside the DMA/compute intervals. Allocation has
  its own interval. DMA intervals may contain barriers; they are not pure
  link-transfer latency.
- The compute interval measures the master's work. Single-cluster and row
  checksums that are printed later remain inside their original intervals.
  Unused grid checksum calculations were removed; grid compute intervals now
  contain only GEMM. This cleanup edit is recorded in the manifest.
- Row checksum printing introduces untimed global barriers before the later
  timed post-compute barrier. That later interval does not capture the whole
  wait for slower workers.
- Grid broadcast output timing ends when the master finishes its store;
  the following final barrier is required to complete the other workers.

Do not add these intervals and label the sum an end-to-end slide reproduction
without reconciling the excluded work and the original measurement method.
See `docs/project/REPRODUCTION.md` from the repository root.

## Correctness and provenance

Legacy additive checksums are diagnostic output, not numerical validation.
The cleanup was checked using a 48-thread host fixture for all five programs
at O0 and O3, comparing delivered A/B data and all output elements. Broadcast
fault tests intentionally omit a receiver and must fail. Host fixture timing
has no performance meaning. Native validation of these exact cleaned files
and the presentation configuration remains pending.

The untouched originals, a manifest, and the complete cleanup diff are in
`docs/project/reproduction/`. The original allocation, barrier, timer, input
pattern, and GEMM arithmetic choices are preserved except for the explicitly
listed preload and broadcast fixes and removal of unused diagnostic code.
