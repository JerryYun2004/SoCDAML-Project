/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 * Timing:
 *   - data_transfer_cycles: L1 alloc + HBM pull + broadcast + wait (no barriers)
 *   - synchronization_cycles: the two global barriers (between data phases)
 * Prints once from C[0,0] DM core using tiny printf (flex_printf.h).
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"   /* provides printf via tiny printf */

/* ---------- tiny helpers ---------- */
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

/* ------------------- one-time HBM init (silent, identical to main_direct) ------------------- */
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

    /* Only the DM core in each cluster should touch DMA/L1 in this microbench */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* ------------- timing accumulators (32-bit cycles) ------------- */
    uint32_t cycles_data = 0u;   /* L1 alloc + HBM pulls + broadcast + wait */
    uint32_t cycles_sync = 0u;   /* the two barriers only */

    
    

    /* ======================== Data phase (part 1) ======================== */
    flex_timer_start(); /* Emit a global stamp window (optional, for external timeline tools) */

    /* -------- L1 allocations (DM core per cluster) -------- */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for 64B alignment margin */
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    /* Compute 64B-aligned TCDM offsets for DMA & broadcast engines */
    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* -------- Leaders pull from HBM -------- */
    if (P.x == 0u) {  /* A-strip: 64x256 contiguous */
        const uint32_t r = P.y;
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }
    if (P.y == 0u) {  /* B-strip: 256x64 strided */
        const uint32_t c = P.x;
        const uint32_t base         = hbm_off_B_strip_base(c);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;
        const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;        /* 256*4 */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                         /* 256   */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(base),
                          /*rowSize*/ size_per_row, /*dstStride*/ dst_stride,
                          /*srcStride*/ src_stride, /*rows*/ repeat);
        bare_dma_wait_all();
    }

    flex_timer_end();
    /* end data phase part 1 */
    
    /* ======================== Sync barrier #1 ======================== */
    flex_timer_start();
    flex_global_barrier_xy();            /* wait until all leaders finished HBM pulls */
    flex_timer_end();
    
    /* ======================== Data phase (part 2) ======================== */
    flex_timer_start();

    /* Inter-cluster broadcasts (only leaders initiate) */
    if (P.x == 0u) {  /* A along row */
        const uint16_t row_m = mask_row(P.y);
        const uint16_t col_m = mask_all4();
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }
    if (P.y == 0u) {  /* B down column */
        const uint16_t row_m = mask_all4();
        const uint16_t col_m = mask_col(P.x);
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }

    /* Wait for any broadcast(s) triggered by this cluster */
    flex_dma_async_wait_all();  /* end data phase part 2 */
    
    flex_timer_end();
    
    /* ======================== Sync barrier #2 ======================== */
    flex_timer_start();
    flex_global_barrier_xy();            /* make sure everyone has both strips */
    flex_timer_end(); /* Emit the global end stamp (optional) */


    flex_eoc(0);
    return 0;
}
