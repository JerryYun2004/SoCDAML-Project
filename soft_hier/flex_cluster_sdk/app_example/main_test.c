// =============================
// File: main_test.c
// =============================
#include <stdint.h>

#include "flex_printf.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* -----------------------------------------------------------
 * Naive C = A(64x256) * B(256x64) on one core (fp32)
 *   - A is row-major [64][256]
 *   - B is row-major [256][64]
 *   - C is row-major [64][64]
 * ----------------------------------------------------------- */
static void matmul_64x256_256x64(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < C_TILE_ROWS; ++i) {
        for (uint32_t j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < A_STRIP_COLS; ++k) { /* 256 */
                acc += A[i * A_STRIP_COLS + k] * B[k * B_STRIP_COLS + j];
            }
            C[i * C_TILE_COLS + j] = acc;
        }
    }
}

int main(void)
{
    /* ---- Bring-up & barriers ---- */
    flex_barrier_xy_init();              /* set up XY barrier */
    flex_global_barrier_xy();

    /* One init per cluster (first core in each cluster). */
    if (flex_is_first_core()) {
        flex_alloc_init();               /* sets up L1+HBM allocators; guarded inside */
    }
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    if (cid == 0 && core == 0) {
        printf("[Info] 4x4 cluster GEMM with inter-cluster broadcast (FP32)
");
        printf("       Grid=(%u x %u), cores/cluster=%u
",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, bytes(A_strip)=%u, bytes(B_strip)=%u, bytes(C_tile)=%u
",
               (unsigned)MAT_N, (unsigned)TILE,
               (unsigned)BYTES_A_STRIP, (unsigned)BYTES_B_STRIP, (unsigned)BYTES_C_TILE);
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
            printf("[ERR][C%u] L1 malloc failed: A=%p B=%p C=%p
",
                   cid, addr_a, addr_b, addr_c);
            flex_eoc(1);
            return 1;
        }

        /* Zero C tile */
        zero_f32(addr_c, BYTES_C_TILE);

        /* Compute and log TCDM offsets (identical across clusters). */
        uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        uint32_t c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);
        printf("[C%u,%u] L1 A_off=0x%08x B_off=0x%08x C_off=0x%08x
",
               P.y, P.x, a_off, b_off, c_off);

        /* ---- Leaders pull their strips from HBM ---- */
        if (P.x == 0u) {
            const uint32_t r = P.y; /* 0..3 */
            const uint32_t offA = hbm_off_A_strip(r);
            printf("[Load][A] C(%u,%u) <- HBM+0x%08x bytes=%u
", P.y, P.x, offA, (unsigned)BYTES_A_STRIP);
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
        }
        if (P.y == 0u) {
            const uint32_t c = P.x; /* 0..3 */
            const uint32_t offB = hbm_off_B_strip_base(c);
            const size_t   size_per_row = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t   dst_stride   = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t   src_stride   = (size_t)MAT_N        * ELEM_BYTES;   /* 256*4 */
            const size_t   repeat       = (size_t)B_STRIP_ROWS;                /* 256   */
            printf("[Load][B] C(%u,%u) <- HBM+0x%08x 2D: size=%u dst_str=%u src_str=%u reps=%u
",
                   P.y, P.x, offB, (unsigned)size_per_row, (unsigned)dst_stride,
                   (unsigned)src_stride, (unsigned)repeat);
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
        }

        /* ---- Inter-cluster broadcasts ---- */
        if (P.x == 0u) {
            const uint16_t row_m = mask_row(P.y);   /* this row */
            const uint16_t col_m = mask_all4();     /* all columns */
            printf("[Bcast][A] from C(%u,%u) row_m=0x%04x col_m=0x%04x bytes=%u
",
                   P.y, P.x, row_m, col_m, (unsigned)BYTES_A_STRIP);
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
        }
        if (P.y == 0u) {
            const uint16_t row_m = mask_all4();     /* all rows */
            const uint16_t col_m = mask_col(P.x);   /* this column */
            printf("[Bcast][B] from C(%u,%u) row_m=0x%04x col_m=0x%04x bytes=%u
",
                   P.y, P.x, row_m, col_m, (unsigned)BYTES_B_STRIP);
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
        }
        flex_dma_async_wait_all();

        /* ---- Compute C_tile on core 0 ---- */
        matmul_64x256_256x64((const float*)addr_a, (const float*)addr_b, (float*)addr_c);
        printf("[Compute][C(%u,%u)] checksum=0x%08x
", P.y, P.x, checksum_u32(addr_c, BYTES_C_TILE));

        /* ---- Store C_tile to HBM ---- */
        const uint32_t offC = hbm_off_C_tile(P.y, P.x);
        const size_t   size_row  = (size_t)C_TILE_COLS * ELEM_BYTES;  /* 64*4 */
        const size_t   dst_str   = (size_t)MAT_N       * ELEM_BYTES;  /* 256*4 */
        const size_t   src_str   = (size_t)C_TILE_COLS * ELEM_BYTES;  /* 64*4 */
        const size_t   reps      = (size_t)C_TILE_ROWS;               /* 64    */
        printf("[Store][C] C(%u,%u) -> HBM+0x%08x 2D size=%u dst_str=%u src_str=%u reps=%u
",
               P.y, P.x, offC, (unsigned)size_row, (unsigned)dst_str,
               (unsigned)src_str, (unsigned)reps);
        bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                          size_row, dst_str, src_str, reps);
        bare_dma_wait_all();
    }

    flex_global_barrier_xy();

    if (cid == 0 && core == 0) {
        printf("[Done] All 16 tiles of C written to HBM at base 0x%08x
",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
