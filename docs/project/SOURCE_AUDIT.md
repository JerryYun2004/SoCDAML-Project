# Source audit and cleanup record

## Provenance

The five originals were fetched from `JerryYun2004/SoCDAML-Project` at `707cc6f0db370d16f608de7b9ff86e9c758d95e9`. Their exact bytes were checked against GitHub blob identities. They are retained under `reproduction/originals/` beside this document.

The package does not replace original scheduling with a shared synthetic main function. It does not claim that this revision generated the presentation plots. The current source defaults remain K=256 scalar FP32. See [REPRODUCTION.md](REPRODUCTION.md) for the slide/source reconciliation.

## Intentional executable edits

### 1. `single_cluster.c`

- Rename compute_2d_gemm to gemm_scalar_fp32; loop body unchanged.
- Use SDK ELEM_BYTES with an FP32 guard instead of redefining it.

The remaining changes are comments and whitespace; a C-token comparison checks that no additional edits entered this source. GEMM loop order, FP32 math, dimensions, input patterns, barrier placement and timer call order remain traceable to the original.

### 2. `row_direct.c`

- Release completed preload buffers before worker allocation; apply to both row variants.
- Rename compute_gemm_tile to gemm_scalar_fp32; loop body unchanged.
- Namespace row C tile macro and remove unused diagnostic variables.

The remaining changes are comments and whitespace; a C-token comparison checks that no additional edits entered this source. GEMM loop order, FP32 math, dimensions, input patterns, barrier placement and timer call order remain traceable to the original.

### 3. `row_broadcast.c`

- Release completed preload buffers before worker allocation; apply to both row variants.
- Use coordinate-match masks for all x at sender y.
- Use SDK offset wrapper to construct full local/remote DMA addresses.
- Rename compute_gemm_tile to gemm_scalar_fp32; loop body unchanged.
- Namespace row C tile macro and remove unused diagnostic variables.

The remaining changes are comments and whitespace; a C-token comparison checks that no additional edits entered this source. GEMM loop order, FP32 math, dimensions, input patterns, barrier placement and timer call order remain traceable to the original.

### 4. `grid_direct.c`

- Rename matmul_64x256_256x64 to gemm_scalar_fp32; loop body unchanged.
- Remove unused grid checksum calculations/helpers and unused offsets; compute interval now contains the kernel only.

The remaining changes are comments and whitespace; a C-token comparison checks that no additional edits entered this source. GEMM loop order, FP32 math, dimensions, input patterns, barrier placement and timer call order remain traceable to the original.

### 5. `grid_broadcast.c`

- Use coordinate-match masks: row (0,15), column (15,0).
- Rename matmul_64x256_256x64 to gemm_scalar_fp32; loop body unchanged.
- Remove unused grid checksum calculations/helpers and unused offsets; compute interval now contains the kernel only.

The remaining changes are comments and whitespace; a C-token comparison checks that no additional edits entered this source. GEMM loop order, FP32 math, dimensions, input patterns, barrier placement and timer call order remain traceable to the original.

## Measurement boundaries

Timer names were added as source comments only, in the following execution order. They do not add serial I/O to measured regions. Only the master C[0,0] DM core emits simulator timer values.

**single_cluster**: 1. `allocation` → 2. `sync_after_allocation` → 3. `sync_after_preload` → 4. `input_dma` → 5. `sync_after_input` → 6. `compute_zero_and_checksum` → 7. `sync_after_compute` → 8. `output_dma` → 9. `final_sync`.

**row_direct**: 1. `allocation` → 2. `input_dma_with_barriers` → 3. `sync_after_input` → 4. `compute_and_checksum` → 5. `sync_after_checksum_prints` → 6. `output_dma_with_barriers` → 7. `final_sync`.

**row_broadcast**: 1. `allocation` → 2. `sync_after_allocation` → 3. `input_dma_and_broadcast_with_barriers` → 4. `sync_after_input` → 5. `compute_and_checksum` → 6. `sync_after_checksum_prints` → 7. `output_dma_with_barriers` → 8. `final_sync`.

**grid_direct**: 1. `allocation_and_zero` → 2. `sync_after_allocation` → 3. `input_dma_with_barriers` → 4. `sync_after_input` → 5. `compute` → 6. `sync_after_compute` → 7. `output_dma_with_barriers` → 8. `final_sync`.

**grid_broadcast**: 1. `allocation_and_zero` → 2. `sync_after_allocation` → 3. `leader_input_dma_with_barriers` → 4. `sync_after_input` → 5. `broadcast_with_barriers` → 6. `sync_after_broadcast` → 7. `compute` → 8. `sync_after_compute` → 9. `parallel_output_dma_master_interval` → 10. `final_sync`.

## Material limitations

- The slides use K=64, Spatz_4, and eight HBM channels. The recovered programs use K=256 scalar loops and the current native model has Spatz disabled and four west channels. Do not label these sources as a completed slide reproduction.
- Adding HBM nodes is insufficient without matching matrix-to-channel addressing. Enabling Spatz is insufficient without a vector kernel and compatible toolchain.
- Original row sources have a global input barrier and untimed ordered checksum-print barriers after compute. The slide notes describe worker-local compute start. Preserve this discrepancy until the historical run source is recovered.
- Grid direct serializes output stores; grid broadcast stores concurrently. Its speedup includes this scheduling difference.
- Original grid checksums were unused. Cleanup removes them, so the grid compute interval now contains the GEMM kernel only. New timings are required; executable/timing equivalence to the old build is not asserted.
- The grid input B is diagonal. It is useful for layout validation but does not substitute for additional dense correctness cases in the final reproduction suite.
- Legacy bit-pattern checksums are not numerical verification and retain the original type-punning implementation where their output is used. The host functional checker independently validates every output; the slide-aligned version should move diagnostics fully outside its agreed timing boundaries.

## Checks completed for this cleanup

All five actual cleaned C files passed at O0 and O3 in a 48-thread host fixture: one/4/16 active workers, exact A/B buffers, and 4,096/16,384/65,536 final HBM outputs respectively. Dropped receiver tests for row and grid broadcast were rejected. Strict source syntax checks passed with `-Wall -Wextra -Werror` against fixture headers. Source hashes and the host-only validation scope are recorded in `reproduction/HOST_VALIDATION.json`.

These checks do not run GVSoC, exercise the native compatibility layer, or measure speedup. Native execution of the cleaned files and reproduction of the historical figures are pending.
