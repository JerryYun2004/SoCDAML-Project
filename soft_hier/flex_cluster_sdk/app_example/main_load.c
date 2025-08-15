/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 *
 * Sections timed (cycles, printed once by C[0,0] DM):
 *   - data_transfer:  L1 alloc + HBM->L1 leader loads + inter-cluster broadcasts
 *   - synchronization: the two flex_global_barrier_xy() calls bracketing the data phase
 *   - compute: 0 (no math here)
 *
 * Preload (excluded from timings):
 *   A: ramp (idx+1)/65536
 *   B: identity with 2.0 on diag columns {0,64,128,192}, else 1.0 on diag
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"     /* for output — no stdio */

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

/* 32-bit cycle counter (avoid cycleh so GVSoC doesn’t warn) */
static inline uint32_t rdcycle32(void)
{
    uint32_t c;
    __asm__ volatile("rdcycle %0" : "=r"(c));
    return c;
}

/* ------------------- one-time HBM init (silent) -------------------
   Matches main_direct: A ramp, B identity with 2.0 on columns 0,64,128,192 */
static void init_hbm_AB_silent(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;  /* only C[0,0] DM core */

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
        dma_write_row((uint32_t)HBM_A_BASE_OFFSET + r * ROW_BYTES, row_off, ROW_BYTES);
    }

    /* B rows: identity; diag=2.0 for columns 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        if ((r % (uint32_t)C_TILE_COLS) == 0u) pf[r] = 2.0f;
        else                                    pf[r] = 1.0f;
        dma_write_row((uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES, row_off, ROW_BYTES);
    }
}

/* ================================ MAIN ================================ */

int main(void)
{
    /* Bring-up */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    /* Alloc subsystem (not timed) */
    flex_alloc_init();
    flex_global_barrier_xy();

    /* Silent HBM preload (excluded from timings) */
    init_hbm_AB_silent();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before timed phase */

    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(flex_get_cluster_id());

    /* Only DM core participates in this microbench */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* ================= timing plan =================
     *
     * [Barrier #1]                measured as sync part (tb1 - tb0)
     *   tb0 = rdcycle32();
     *   flex_global_barrier_xy();
     *   tb1 = rdcycle32();
     *
     * [DATA TRANSFER window]      measured as data part (tb2 - tb1)
     *   - L1 allocations
     *   - leader loads from HBM
     *   - broadcasts + wait
     *   tb2 = rdcycle32();
     *
     * [Barrier #2]                measured as sync part (tb3 - tb2)
     *   flex_global_barrier_xy();
     *   tb3 = rdcycle32();
     *
     * compute = 0
     * ================================================ */

    uint32_t tb0, tb1, tb2, tb3;
    uint32_t cycles_sync, cycles_data, cycles_compute = 0u;

    /* -------- Barrier #1 (start sync timing) -------- */
    tb0 = rdcycle32();
    flex_global_barrier_xy();
    tb1 = rdcycle32();

    /* ================= DATA TRANSFER window ================= */
    /* L1 allocations (DM-core per cluster; included in data) */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Leaders pull from HBM */
    if (P.x == 0u) {
        const uint32_t r = P.y;  /* row leader loads its A strip (64x256) */
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }
    if (P.y == 0u) {
        const uint32_t c = P.x;  /* column leader loads its B strip (256x64) */
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

    /* Broadcasts (only leaders initiate) */
    if (P.x == 0u) {
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, mask_row(P.y), mask_all4());
    }
    if (P.y == 0u) {
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, mask_all4(),   mask_col(P.x));
    }
    flex_dma_async_wait_all();  /* end of data-transfer work */
    /* ================= end DATA TRANSFER window ================= */

    tb2 = rdcycle32();

    /* -------- Barrier #2 (end sync timing) -------- */
    flex_global_barrier_xy();
    tb3 = rdcycle32();

    /* Accumulate times */
    cycles_sync  = (tb1 - tb0) + (tb3 - tb2);
    cycles_data  = (tb2 - tb1);
    /* cycles_compute = 0 */

    /* ------------------ print once from C[0,0] DM ------------------ */
    if (P.x == 0u && P.y == 0u) {
        flex_printf("[Timing] data_transfer_cycles=%u\n", (unsigned)cycles_data);
        flex_printf("[Timing] synchronization_cycles=%u\n", (unsigned)cycles_sync);
        flex_printf("[Timing] compute_cycles=%u\n", (unsigned)cycles_compute);
    }

    /* final sync not needed; print is from single node */
    flex_eoc(0);
    return 0;
}
