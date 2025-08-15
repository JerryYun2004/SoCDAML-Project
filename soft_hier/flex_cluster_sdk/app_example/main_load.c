/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload + timing)
 *
 * What it does safely:
 *   1) Every cluster allocates the same 64B-aligned SCRATCH row buffer (ROW_BYTES+64).
 *      Only C[0,0] actually uses its scratch to silently preload A/B rows to HBM.
 *      We do NOT free this scratch to keep L1 allocation history identical.
 *   2) Every cluster allocates its A and B buffers (same order/sizes everywhere),
 *      so a_off/b_off are identical across the 4×4 grid.
 *   3) Row leaders (x==0) load A (1D); column leaders (y==0) load B (2D).
 *   4) Leaders broadcast A along rows, B down columns; dst_off matches receivers.
 *   5) We time data-transfer and synchronization; compute=0.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"    /* printf(...) provided here; no stdio */

/* ---------- helpers ---------- */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void *)v;
}

static inline uint32_t rdcycle32(void)
{
    uint32_t c;
    asm volatile("csrr %0, cycle" : "=r"(c));
    return c;
}

static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* Silent HBM preload on C[0,0] using a pre-allocated scratch row at row_off. */
static void init_hbm_AB_silent_with(uint32_t row_off)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const FlexPosition P = get_pos(flex_get_cluster_id());
    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES; /* 256*4 = 1024 */

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

    /* B rows: identity; diag=2.0 on columns 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        if ((r % (uint32_t)C_TILE_COLS) == 0u) pf[r] = 2.0f; else pf[r] = 1.0f;
        const uint32_t offB_row = (uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }
}

/* =============================== MAIN =============================== */
int main(void)
{
    /* Bring-up */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    /* Optional: open a coarse external timer window (C[0,0]) */
    if (flex_get_cluster_id() == 0u && flex_get_core_id() == 0u) {
        flex_timer_start();
    }
    flex_global_barrier_xy();

    /* All clusters init allocator BEFORE any per-cluster allocations */
    flex_alloc_init();
    flex_global_barrier_xy();

    /* We’ll time three sections (compute is 0 in this microbench) */
    uint32_t cycles_data = 0u;
    uint32_t cycles_sync = 0u;
    const uint32_t cycles_compute = 0u;

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    /* Only DM core touches DMA/L1; other cores exit quietly */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* =================== PHASE 0: symmetric scratch alloc (all clusters) =================== */
    const uint32_t ROW_BYTES = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES; /* 1024 */
    void *scratch_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (scratch_raw == (void*)0) { flex_eoc(1); return 0; }
    void *scratch_aln = align64(scratch_raw);
    uint32_t scratch_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, scratch_aln);
    scratch_off = align_up_u32(scratch_off, 64u);

    /* Only C[0,0] uses its scratch to preload A/B into HBM; others keep it reserved */
    init_hbm_AB_silent_with(scratch_off);
    flex_global_barrier_xy();   /* A/B guaranteed in HBM now */

    /* =================== PHASE 1: A/B alloc + leader loads (data_transfer) =================== */
    uint32_t t0 = rdcycle32();

    /* Per-cluster A/B buffers; identical order/sizes → identical offsets across grid */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Leaders pull from HBM */
    if (P.x == 0u) {
        const uint32_t r = P.y;
        const uint32_t h_off = hbm_off_A_strip(r); /* contiguous 64x256 */
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }
    if (P.y == 0u) {
        const uint32_t c = P.x;
        const uint32_t base         = hbm_off_B_strip_base(c);         /* strided 256x64 */
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;                                   /* packed */
        const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;         /* 256*4  */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                          /* 256    */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(base),
                          /*rowSize*/ size_per_row, /*dstStride*/ dst_stride,
                          /*srcStride*/ src_stride, /*rows*/ repeat);
        bare_dma_wait_all();
    }

    uint32_t t1 = rdcycle32();
    cycles_data += (t1 - t0);   /* includes A/B allocations + leader loads */

    /* =================== PHASE 2: barrier before broadcasts (synchronization) =================== */
    uint32_t s0 = rdcycle32();
    flex_global_barrier_xy();
    uint32_t s1 = rdcycle32();
    cycles_sync += (s1 - s0);

    /* =================== PHASE 3: broadcasts (data_transfer) =================== */
    uint32_t t2 = rdcycle32();

    if (P.x == 0u) {
        const uint16_t row_m = mask_row(P.y);  /* broadcast along this row */
        const uint16_t col_m = mask_all4();    /* to all columns           */
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }
    if (P.y == 0u) {
        const uint16_t row_m = mask_all4();    /* to all rows */
        const uint16_t col_m = mask_col(P.x);  /* this column */
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }
    flex_dma_async_wait_all();

    uint32_t t3 = rdcycle32();
    cycles_data += (t3 - t2);   /* add broadcast time */

    /* =================== PHASE 4: final sync (synchronization) =================== */
    uint32_t s2 = rdcycle32();
    flex_global_barrier_xy();
    uint32_t s3 = rdcycle32();
    cycles_sync += (s3 - s2);

    /* =================== Final report (exactly once) =================== */
    flex_global_barrier_xy();
    if (cid == 0u && core == 0u) {
        printf("[Timing] data_transfer_cycles=%u\n", (unsigned)cycles_data);
        printf("[Timing] synchronization_cycles=%u\n", (unsigned)cycles_sync);
        printf("[Timing] compute_cycles=%u\n", (unsigned)0u);
        flex_timer_end();
    }
    flex_global_barrier_xy();

    flex_eoc(0);
    return 0;
}
