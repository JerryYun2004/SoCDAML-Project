/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 *
 * Sections measured:
 *   - DATA  : L1 alloc + leaders pull from HBM + broadcasts + DMA waits
 *   - SYNC  : the two global barriers (before and after broadcast)
 *   - COMPUTE: none (reported as 0)
 *
 * Notes:
 *   - Uses only 32-bit 'cycle' CSR (no 'cycleh'), so GVSoC won't warn.
 *   - Also tags sections with flex_timer_start/stop (sim-side markers).
 *   - Prints results via flex_print / flex_print_int (no stdio).
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"   /* MAT_N, ELEM_BYTES, C_TILE_COLS, BYTES_*,
                             HBM_*_BASE_OFFSET, mask_row/col/all4, hbm_off_*,
                             align_up_u32, local(), hbm_addr(), etc. */

/* ------------------------ tiny helpers ------------------------ */
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
static inline uint32_t rdcycle32(void)
{
    uint32_t c;
    __asm__ volatile ("csrr %0, cycle" : "=r"(c));
    return c;
}
static inline uint32_t cyc_delta32(uint32_t start, uint32_t end)
{
    /* works across wrap for 32-bit counters */
    return (uint32_t)(end - start);
}
static inline void pdec(const char *s, uint32_t v)
{
    /* tiny print: string then decimal then newline */
    while (*s) { flex_log_char(*s++); }
    flex_print_int(v);
    flex_log_char('\n');
}

/* ------------------- one-time HBM init (silent, same as direct) -------------------
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

    /* Silent HBM preload (only C[0,0] DM executes) */
    init_hbm_AB_silent();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before any load */

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */
    const uint32_t IS_DM = flex_is_dm_core();

    /* Only the DM core in each cluster should touch DMA/L1 in this microbench */
    if (!IS_DM) {
        /* stay alive but idle until the end to avoid affecting barriers */
        flex_global_barrier_xy();
        flex_global_barrier_xy();
        flex_eoc(0);
        return 0;
    }

    /* -------- L1 allocations (DM core per cluster) -------- */
    uint32_t cyc_data_s=0, cyc_data_e=0;
    uint32_t cyc_sync_s=0, cyc_sync_e=0;
    uint32_t cyc_compute = 0;

    /* DATA section timing (includes alloc + HBM loads + broadcasts + waits) */
    cyc_data_s = rdcycle32();
    flex_timer_start();                 /* simulator-side mark: DATA begin */

    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for 64B alignment margin */
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_timer_stop(); flex_eoc(1); return 0; }

    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* SYNC section: barrier to ensure leaders finished HBM pulls happens AFTER the pulls,
       but we time barriers separately, so we’ll stop DATA just before first barrier. */

    /* Leaders pull from HBM (part of DATA) */
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

    /* End of DATA part 1; SYNC barrier #1 begins next */
    cyc_data_e = rdcycle32();
    flex_timer_stop();                   /* simulator-side mark: DATA end */

    /* SYNC section (barrier #1) */
    cyc_sync_s = rdcycle32();
    flex_timer_start();                  /* simulator-side mark: SYNC begin */
    flex_global_barrier_xy();            /* wait all leaders finished HBM pulls */

    /* Back to DATA: broadcasts + waits (still counted inside DATA by requirement) */
    flex_timer_stop();                   /* end SYNC #1 */
    flex_timer_start();                  /* re-open DATA window */

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
    flex_dma_async_wait_all();           /* ensure broadcasts complete */

    /* End of DATA part 2; SYNC barrier #2 next */
    cyc_data_e = rdcycle32();
    flex_timer_stop();                   /* simulator-side mark: DATA end (final) */

    /* SYNC section (barrier #2) */
    {
        uint32_t s2 = rdcycle32();
        flex_timer_start();
        flex_global_barrier_xy();        /* all clusters have both strips */
        flex_timer_stop();
        uint32_t e2 = rdcycle32();
        /* accumulate both sync barriers: cyc_sync = (barrier #1) + (barrier #2) */
        cyc_sync_e = e2;
        /* Add first barrier cost (we measured it in cyc_sync_s..cyc_sync_e previously) */
        /* For simplicity: reuse cyc_sync_s/e as the *total* sync time by adding deltas. */
        uint32_t sync1 = cyc_delta32(cyc_sync_s, cyc_sync_e); /* from first SYNC block */
        uint32_t sync2 = cyc_delta32(s2, e2);
        cyc_sync_s = 0;                   /* repurpose fields to store total */
        cyc_sync_e = sync1 + sync2;
    }

    /* COMPUTE section (none) */
    cyc_compute = 0u;

    /* Report once from C[0,0] to avoid interleaved prints */
    if (P.x == 0u && P.y == 0u) {
        uint32_t t_data = cyc_delta32(cyc_data_s, cyc_data_e);
        uint32_t t_sync = cyc_sync_e;  /* total from both barriers */
        uint32_t t_comp = cyc_compute;

        pdec("[TIMER] data_cycles=", t_data);
        pdec("[TIMER] sync_cycles=", t_sync);
        pdec("[TIMER] compute_cycles=", t_comp);
    }

    flex_eoc(0);
    return 0;
}
