/* main_load.c — HBM→L1 load test with row/col broadcast (no prints) */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "soc_daml.h"

static inline void ml_zero32(void *dst, uint32_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)dst;
    for (uint32_t i = 0; i < (bytes >> 2); ++i) p[i] = 0u;
}

int main(void)
{
    flex_barrier_xy_init();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t cx = P.x;  /* 0..3 */
    const uint32_t cy = P.y;  /* 0..3 */

    /* DM core allocates L1 slots for A/B. */
    void *addr_a = 0, *addr_b = 0;
    uint32_t a_off = 0, b_off = 0;

    if (core == 0) {
        /* +64 to ensure we can 64B-align the usable region. */
        addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);

        if (!addr_a || !addr_b) {
            flex_eoc(1);
        }

        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);

        /* 64B align the working offsets (avoid helper name collisions). */
        a_off = (a_off + 63u) & ~63u;
        b_off = (b_off + 63u) & ~63u;

        /* Clear destinations (optional; helps catch stale data internally). */
        ml_zero32((void*)local(a_off), BYTES_A_STRIP);
        ml_zero32((void*)local(b_off), BYTES_B_STRIP);
    }
    flex_global_barrier_xy();

    /* Leaders load from HBM (DM core only). */
    if (core == 0) {
        /* Row leaders (cx==0) load A-row strip (contiguous). */
        if (cx == 0u) {
            const uint32_t offA = hbm_off_A_strip(cy); /* 64x256 fp32 = 64*256*4 */
            bare_dma_start_1d(local(a_off), hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
        }

        /* Column leaders (cy==0) load B-column strip (2D gather). */
        if (cy == 0u) {
            const uint32_t baseB       = hbm_off_B_strip_base(cx);
            const uint32_t size_per_row = TILE * ELEM_BYTES;   /* 64*4 */
            const uint32_t dst_stride   = size_per_row;        /* tightly packed in L1 */
            const uint32_t src_stride   = MAT_N * ELEM_BYTES;  /* 256*4 in HBM */
            bare_dma_start_2d(local(b_off), hbm_addr(baseB),
                              size_per_row, dst_stride, src_stride, MAT_N);
            bare_dma_wait_all();
        }
    }
    /* Ensure all leaders finished HBM reads before any broadcast starts. */
    flex_global_barrier_xy();

    /* Inter-cluster broadcast (DM core only). */
    if (core == 0) {
        /* Row broadcast of A from (cx==0) across columns. */
        if (cx == 0u) {
            const uint16_t row_m = mask_row(cy);
            const uint16_t col_m = mask_all4();   /* to all columns */
            flex_dma_async_broadcast(a_off, a_off, BYTES_A_STRIP, row_m, col_m);
        }
        /* Column broadcast of B from (cy==0) across rows. */
        if (cy == 0u) {
            const uint16_t row_m = mask_all4();   /* to all rows    */
            const uint16_t col_m = mask_col(cx);
            flex_dma_async_broadcast(b_off, b_off, BYTES_B_STRIP, row_m, col_m);
        }
        flex_dma_async_wait_all();
    }

    /* All clusters must wait until both row/column broadcasts are done. */
    flex_global_barrier_xy();

    /* Done: no compute, no store, just load/broadcast timing. */
    flex_eoc(0);
    return 0;
}
