/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 *
 * Phases measured (no prints):
 *   SYNC_PRE    : barrier between “leaders pull” and “broadcast”
 *   XFER_PULL   : leaders’ HBM->L1 pulls (A rows & B columns)
 *   XFER_BCAST  : inter-cluster broadcasts (row for A, column for B)
 *   SYNC_POST   : final barrier after all transfers
 *
 * Notes:
 * - HBM is preloaded once by C[0,0] with the same data pattern as main_direct.c:
 *     A: ramp (idx+1)/65536
 *     B: identity with 2.0 on diagonal columns {0,64,128,192}, else 1.0
 * - Only DM core (core==0) in each cluster touches L1/DMA.
 * - Only C[0,0] DM toggles the global timer registers.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"   /* MAT_N, ELEM_BYTES, BYTES_*, HBM_*_BASE_OFFSET,
                             hbm_off_*, mask_row/col/all4, align_up_u32,
                             local(), hbm_addr(), etc. */

/* --- tiny helpers (no std headers) --- */
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

/* ------------------- one-time HBM init (silent, identical to main_direct) -------------------
   A: ramp (idx+1)/65536;  B: identity with 2.0 on diag columns {0,64,128,192}. */
static void init_hbm_AB_silent(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    /* Only C[0,0] DM does the pre-load */
    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES; /* 256*4 = 1024 */

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) { flex_eoc(1); return; }

    void *row_aln         = align64(row_raw);
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

    /* Silent HBM preload (only C[0,0] DM executes) */
    init_hbm_AB_silent();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before any load */

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */

    const uint32_t IS_DM      = (core == 0u);
    const uint32_t IS_MASTER  = (cid == 0u) && (core == 0u); /* only C[0,0] DM touches the timer */

    /* Only the DM core in each cluster should touch DMA/L1 in this microbench */
    if (!IS_DM) { flex_eoc(0); return 0; }

    /* -------- L1 allocations (DM core per cluster) -------- */
    /* We include allocation time in the data-transfer windows below. */
    void *addr_a = 0, *addr_b = 0;
    uint32_t a_off = 0u, b_off = 0u;

    /* ====================== XFER_PULL (leaders’ HBM loads) ====================== */
    flex_global_barrier_xy();                 /* align section start across clusters */
    if (IS_MASTER) flex_timer_start();        /* XFER_PULL start */

    addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for 64B align margin */
    addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);

    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Leaders pull from HBM (A rows by row leaders; B columns by column leaders) */
    /* A-strip: row leader (x==0) pulls its row’s 64x256 (1D contiguous). */
    if (P.x == 0u) {
        const uint32_t r = P.y;  /* 0..3 */
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }

    /* B-strip: column leader (y==0) pulls its column’s 256x64 (2D strided). */
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

    if (IS_MASTER) flex_timer_end();          /* XFER_PULL end */

    /* =========================== SYNC_PRE (barrier) ============================ */
    if (IS_MASTER) flex_timer_start();        /* SYNC_PRE start (barrier only) */
    flex_global_barrier_xy();                 /* ensure all leaders have finished pulls */
    if (IS_MASTER) flex_timer_end();          /* SYNC_PRE end */

    /* ===================== XFER_BCAST (row/col broadcasts) ===================== */
    if (IS_MASTER) flex_timer_start();        /* XFER_BCAST start */

    /* Broadcast A across rows from x==0 leaders */
    if (P.x == 0u) {
        const uint16_t row_m = mask_row(P.y);  /* select this row */
        const uint16_t col_m = mask_all4();    /* all columns     */
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }

    /* Broadcast B down columns from y==0 leaders */
    if (P.y == 0u) {
        const uint16_t row_m = mask_all4();    /* all rows        */
        const uint16_t col_m = mask_col(P.x);  /* select this col */
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }

    flex_dma_async_wait_all();                 /* wait only if we launched any */
    if (IS_MASTER) flex_timer_end();           /* XFER_BCAST end */

    /* =========================== SYNC_POST (barrier) =========================== */
    if (IS_MASTER) flex_timer_start();         /* SYNC_POST start (barrier only) */
    flex_global_barrier_xy();                  /* everyone has A & B strips now */
    if (IS_MASTER) flex_timer_end();           /* SYNC_POST end */

    /* No compute section in this microbench (intentionally empty) */

    flex_eoc(0);
    return 0;
}
