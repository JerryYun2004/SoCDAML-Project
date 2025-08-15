/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload + timing)
 *
 * What it does:
 *   1) One-time, silent HBM init on C[0,0] (same data pattern as main_direct.c):
 *        - A: ramp (idx+1)/65536
 *        - B: identity with 2.0 on diagonal columns {0,64,128,192}, else 1.0 on diag
 *   2) Row leaders C[y,0] load 64x256 A-strips; column leaders C[0,x] load 256x64 B-strips (2D).
 *   3) Leaders broadcast their strips across the row/column.
 *   4) No compute.
 *   5) Measure and print cycles for: DATA, SYNC, COMP(=0).
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"   /* MAT_N, ELEM_BYTES, C_TILE_COLS, BYTES_*,
                             HBM_*_BASE_OFFSET, mask_row/col/all4, hbm_off_*,
                             align_up_u32, local(), hbm_addr(), etc. */

/* ---------- tiny helpers (no std headers) ---------- */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void *)v;
}
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}
static inline uint64_t read_cycles(void)
{
    /* Robust 64-bit cycle read for RV32: read hi, lo, hi and retry if rollover */
    uint32_t hi1, lo, hi2;
    do {
        asm volatile ("csrr %0, cycleh" : "=r"(hi1));
        asm volatile ("csrr %0, cycle"  : "=r"(lo));
        asm volatile ("csrr %0, cycleh" : "=r"(hi2));
    } while (hi1 != hi2);
    return ((uint64_t)hi1 << 32) | (uint64_t)lo;
}

/* ------------------- one-time HBM init (silent, identical to main_direct) -------------------
   A: ramp (idx+1)/65536;  B: identity with 2.0 on diag columns {0,64,128,192}. */
static void init_hbm_AB_silent(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;  /* only C[0,0] DM core does this */

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES; /* 256*4 = 1024 */

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) { flex_eoc(1); return; }

    void *row_aln   = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A rows: ramp (idx+1)/65536 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) {
            uint32_t idx = r * (uint32_t)MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = (uint32_t)HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* B rows: identity; diag=2.0 for columns 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        if ((r % (uint32_t)C_TILE_COLS) == 0u) pf[r] = 2.0f;  /* 0,64,128,192 */
        else                                    pf[r] = 1.0f;
        const uint32_t offB_row = (uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }
}

/* =================================== MAIN =================================== */
int main(void)
{
    /* Bring-up / alloc */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    /* Silent HBM preload (only C[0,0] DM executes) — NOT timed */
    init_hbm_AB_silent();
    /* SYNC #0 (preload fence) — count in SYNC, not DATA */
    uint64_t sync_cycles = 0;
    uint64_t t_sync_s = read_cycles();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before any load */
    sync_cycles += (read_cycles() - t_sync_s);

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */

    /* Only the DM core in each cluster should touch DMA/L1 in this microbench */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* -------- DATA section begins (allocs + HBM loads + broadcast) -------- */
    flex_timer_start(0); /* DATA window for external tools */
    uint64_t data_cycles = 0;

    uint64_t t_data_s = read_cycles();

    /* L1 allocations (included in DATA) */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for 64B alignment margin */
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_timer_end(0); flex_eoc(1); return 0; }

    /* Compute 64B-aligned TCDM offsets for DMA & broadcast engines */
    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Leaders pull from HBM (still in DATA window) */
    if (P.x == 0u) {
        const uint32_t r = P.y;  /* 0..3 */
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }
    if (P.y == 0u) {
        const uint32_t c = P.x;  /* 0..3 */
        const uint32_t base         = hbm_off_B_strip_base(c);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;                                   /* packed */
        const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;         /* 256*4  */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                          /* 256    */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(base),
                          /*rowSize*/ size_per_row, /*dstStride*/ dst_stride,
                          /*srcStride*/ src_stride, /*rows*/ repeat);
        bare_dma_wait_all();
    }

    data_cycles += (read_cycles() - t_data_s);

    /* SYNC #1 (between HBM load and broadcast) — measure separately */
    flex_timer_start(1); /* SYNC window for external tools (coarse) */
    t_sync_s = read_cycles();
    flex_global_barrier_xy();
    sync_cycles += (read_cycles() - t_sync_s);
    flex_timer_end(1);   /* end of first sync window */

    /* DATA resumes: launch broadcasts + local wait */
    t_data_s = read_cycles();

    if (P.x == 0u) {
        const uint16_t row_m = mask_row(P.y);  /* select row P.y */
        const uint16_t col_m = mask_all4();    /* all columns     */
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }
    if (P.y == 0u) {
        const uint16_t row_m = mask_all4();    /* all rows       */
        const uint16_t col_m = mask_col(P.x);  /* select col P.x */
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }
    flex_dma_async_wait_all();

    data_cycles += (read_cycles() - t_data_s);

    /* SYNC #2 (after broadcasts) — measure separately and also close SYNC window */
    flex_timer_start(1); /* reuse SYNC id for external tools (second barrier) */
    t_sync_s = read_cycles();
    flex_global_barrier_xy();
    sync_cycles += (read_cycles() - t_sync_s);
    flex_timer_end(1);

    flex_timer_end(0);   /* end DATA window */

    /* COMP = 0 here (no compute) */
    flex_timer_start(2);
    flex_timer_end(2);

    /* ---------- print timings once (C[0,0] DM) ---------- */
    if (flex_get_cluster_id() == 0u && flex_get_core_id() == 0u) {
        flex_print("[Time] Sections (cycles):\n");

        flex_print("  DATA (alloc + HBM load + bcast): ");
        flex_print_int((uint32_t)(data_cycles >> 32));  /* hi */
        flex_print(" | ");
        flex_print_int((uint32_t)(data_cycles & 0xFFFFFFFFu));  /* lo */
        flex_print("\n");

        flex_print("  SYNC (sum of 2 barriers):       ");
        flex_print_int((uint32_t)(sync_cycles >> 32));  /* hi */
        flex_print(" | ");
        flex_print_int((uint32_t)(sync_cycles & 0xFFFFFFFFu));  /* lo */
        flex_print("\n");

        flex_print("  COMP (none here):               0\n");
    }

    flex_eoc(0);
    return 0;
}
