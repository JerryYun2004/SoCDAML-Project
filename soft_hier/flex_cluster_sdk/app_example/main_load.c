/* Load-only microbenchmark (broadcast):
 * - C[x,0] loads A-row strip r=y from HBM, broadcasts along its row
 * - C[0,y] loads B-col strip c=x from HBM, broadcasts along its column
 * - No compute, no printf: clean timing of HBM->L1 + broadcast
 */
#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h" 

int main(void)
{
    /* Global init that every core must perform */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */

    /* Only core 0 in each cluster continues; others exit now to avoid touching DMA/L1 */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* -------- L1 allocations (shared per cluster; only DM core allocates) -------- */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for alignment headroom */
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);

    if (addr_a == 0 || addr_b == 0) {
        /* Out of L1: abort this cluster safely */
        flex_eoc(1);
        return 0;
    }

    /* Compute 64B-aligned TCDM offsets to use with DMA/broadcast helpers */
    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Optional: clear destination buffers (not required for the benchmark)
       zero_f32((void *)(local(a_off)), BYTES_A_STRIP);
       zero_f32((void *)(local(b_off)), BYTES_B_STRIP);
    */

    /* -------- Leaders pull from HBM -------- */
    /* A-strip: row leader of each row (x==0) pulls its row's 64x256 */
    if (P.x == 0u) {
        const uint32_t r = P.y; /* 0..3 -> A1..A4 row strips */
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }

    /* B-strip: column leader of each column (y==0) pulls its column's 256x64 (strided) */
    if (P.y == 0u) {
        const uint32_t c = P.x; /* 0..3 -> B1..B4 col strips */
        const uint32_t base = hbm_off_B_strip_base(c);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES;   /* 64*4 */
        const uint32_t dst_stride   = size_per_row;                                     /* packed */
        const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;           /* 256*4 */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                            /* 256    */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(base),
                          /*rowSize*/ size_per_row, /*dstStride*/ dst_stride,
                          /*srcStride*/ src_stride, /*rows*/ repeat);
        bare_dma_wait_all();
    }

    /* Make sure all leaders finished HBM pulls before broadcasting */
    flex_global_barrier_xy();

    /* -------- Inter-cluster broadcast (only leaders initiate) -------- */
    /* Broadcast A along rows: row mask = this row, col mask = all cols */
    if (P.x == 0u) {
        const uint16_t row_m = mask_row(P.y);   /* select row P.y */
        const uint16_t col_m = mask_all4();     /* all columns     */
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }

    /* Broadcast B along columns: row mask = all rows, col mask = this col */
    if (P.y == 0u) {
        const uint16_t row_m = mask_all4();     /* all rows        */
        const uint16_t col_m = mask_col(P.x);   /* select col P.x  */
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }

    /* Wait for both A- and B-broadcasts (if any were launched by this cluster) */
    flex_dma_async_wait_all();

    /* Ensure everyone has the strips before ending */
    flex_global_barrier_xy();

    flex_eoc(0);
    return 0;
}
