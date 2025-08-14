#include "flex_runtime.h"
#include "flex_printf.h"
#include "fix_proj.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include <stdint.h>



/* -----------------------------------------------------------
 * Naive C = A(64x256) * B(256x64) on one core (fp32)
 * Layout:
 *   - A is row-major [64][256]
 *   - B is row-major [256][64]
 *   - C is row-major [64][64]
 * ----------------------------------------------------------- */
static void matmul_64x256_256x64(const float *A, const float *B, float *C)
{
    uint32_t i, j, k;
    for (i = 0; i < C_TILE_ROWS; ++i) {
        for (j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (k = 0; k < A_STRIP_COLS; ++k) { /* 256 */
                acc += A[i * A_STRIP_COLS + k] * B[k * B_STRIP_COLS + j];
            }
            C[i * C_TILE_COLS + j] = acc;
        }
    }
}

int main(void)
{
    /* ---- Bring-up & barriers ---- */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    /* Initialize L1 & HBM allocators (first core per cluster performs init) */
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    if (cid == 0 && core == 0) {
        printf("[Info] 4x4 cluster GEMM with inter-cluster broadcast (FP32)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
    }
    flex_global_barrier_xy();

    /* ---- Allocate L1 buffers on EVERY cluster (same sizes, same order) ---- */
    void *addr_a = NULL;  /* 64x256 */
    void *addr_b = NULL;  /* 256x64 */
    void *addr_c = NULL;  /* 64x64  */

    if (core == 0) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP);
        addr_b = flex_l1_malloc(BYTES_B_STRIP);
        addr_c = flex_l1_malloc(BYTES_C_TILE);

        if (!addr_a || !addr_b || !addr_c) {
            printf("[ERR][C%u] L1 malloc failed: A=%p B=%p C=%p\n",
                   cid, addr_a, addr_b, addr_c);
            flex_eoc(1);
            return 1;
        }

        /* Zero C tile */
        zero_f32(addr_c, BYTES_C_TILE);
    }
    flex_global_barrier_xy();

    /* Compute TCDM offsets (must be identical across clusters). */
    uint32_t a_off = 0, b_off = 0, c_off = 0;
    if (core == 0) {
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);
    }
    flex_global_barrier_xy();

    /* ---- Leaders pull their strips from HBM ---- */
    if (core == 0) {
        /* Row leader: x==0 owns A_r (horizontal strip for row r=y). */
        if (P.x == 0u) {
            const uint32_t r = P.y; /* 0..3 */
            const uint32_t hbm_off = hbm_off_A_strip(r);
            /* 1D copy: A strip is contiguous */
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(hbm_off), BYTES_A_STRIP);
            bare_dma_wait_all();
        }

        /* Column leader: y==0 owns B_c (vertical strip for col c=x). */
        if (P.y == 0u) {
            const uint32_t c = P.x; /* 0..3 */
            const uint32_t hbm_off = hbm_off_B_strip_base(c);
            /* 2D copy: per row 64 elems, 256 rows, strides as documented */
            const size_t size_per_row = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t dst_stride   = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t src_stride   = (size_t)MAT_N        * ELEM_BYTES;   /* 256*4 */
            const size_t repeat       = (size_t)B_STRIP_ROWS;                /* 256   */
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(hbm_off),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
        }
    }
    flex_global_barrier_xy();

    /* ---- Inter-cluster broadcasts ---- */
    if (core == 0) {
        /* Horizontal broadcast of A_r from (x=0,y=r) to all x in that row. */
        if (P.x == 0u) {
            const uint16_t row_m = mask_row(P.y);   /* select this row */
            const uint16_t col_m = mask_all4();     /* all columns */
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
        }

        /* Vertical broadcast of B_c from (x=c,y=0) to all y in that column. */
        if (P.y == 0u) {
            const uint16_t row_m = mask_all4();     /* all rows */
            const uint16_t col_m = mask_col(P.x);   /* select this column */
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
        }

        flex_dma_async_wait_all();
    }
    flex_global_barrier_xy();

    /* ---- Compute C_tile on core 0 of each cluster ---- */
    if (core == 0) {
        float *A = (float *)(addr_a);
        float *B = (float *)(addr_b);
        float *C = (float *)(addr_c);
        matmul_64x256_256x64(A, B, C);
    }
    flex_global_barrier_xy();

    /* ---- Store C_tile to HBM at its grid position (y=P.y, x=P.x) ---- */
    if (core == 0) {
        const uint32_t hbm_off_c = hbm_off_C_tile(P.y, P.x);
        const size_t   size_row  = (size_t)C_TILE_COLS * ELEM_BYTES;  /* 64*4 */
        const size_t   dst_str   = (size_t)MAT_N       * ELEM_BYTES;  /* 256*4 */
        const size_t   src_str   = (size_t)C_TILE_COLS * ELEM_BYTES;  /* 64*4 */
        const size_t   reps      = (size_t)C_TILE_ROWS;               /* 64    */

        bare_dma_start_2d(/*dst*/ hbm_addr(hbm_off_c), /*src*/ local(c_off),
                          size_row, dst_str, src_str, reps);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    if (cid == 0 && core == 0) {
        printf("[Done] All 16 tiles of C written to HBM at base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
