#include <stdint.h>
#include "flex_printf.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* -----------------------------------------------------------
 * Naive C = A(64x256) * B(256x64) on one core (fp32)
 * Layout:
 *   - A is row-major [64][256]
 *   - B is row-major [256][64]
 *   - C is row-major [64][64]
 * ----------------------------------------------------------- */
static void matmul_tile_fp32(const float *A, const float *B, float *C)
{
    uint32_t i, j, k;
    for (i = 0; i < C_TILE_ROWS; ++i) {
        for (j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (k = 0; k < A_STRIP_COLS; ++k) { /* 256 */
                acc += A[i * A_STRIP_COLS + (uint32_t)k] * B[(uint32_t)k * B_STRIP_COLS + j];
            }
            C[i * C_TILE_COLS + j] = acc;
        }
    }
}

int main(void)
{
    /* --- Bring-up and global sync --- */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    flex_alloc_init();  /* allocator prints once via SDK */

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);  /* P.x = col (0..3), P.y = row (0..3) */

    if (cid == 0u && core == 0u) {
        printf("[Info] 4x4 cluster GEMM with inter-cluster broadcast (FP32)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               (unsigned)ARCH_NUM_CLUSTER_X, (unsigned)ARCH_NUM_CLUSTER_Y,
               (unsigned)ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem_bytes=%u\n",
               (unsigned)MAT_N, (unsigned)TILE, (unsigned)ELEM_BYTES);
        printf("       A-strip bytes=%u, B-strip bytes=%u, C-tile bytes=%u\n",
               (unsigned)BYTES_A_STRIP, (unsigned)BYTES_B_STRIP, (unsigned)BYTES_C_TILE);
    }

    /* --- Allocate identical L1 buffers on every cluster (core 0 only) --- */
    void *addr_a = (void*)0;
    void *addr_b = (void*)0;
    void *addr_c = (void*)0;

    if (core == 0u) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP); /* 64x256 */
        addr_b = flex_l1_malloc(BYTES_B_STRIP); /* 256x64 */
        addr_c = flex_l1_malloc(BYTES_C_TILE);  /* 64x64  */
        if (addr_a == (void*)0 || addr_b == (void*)0 || addr_c == (void*)0) {
            if (cid == 0u && core == 0u) {
                printf("[Error] L1 allocation failed (A=%p, B=%p, C=%p)\n", addr_a, addr_b, addr_c);
            }
            flex_eoc(1); return 1;
        }
        zero_f32(addr_c, BYTES_C_TILE);
    }
    flex_global_barrier_xy();

    /* Compute TCDM offsets (used by collective DMA) */
    uint32_t a_off = 0u, b_off = 0u, c_off = 0u;
    if (core == 0u) {
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);
        printf("[C%u,%u] L1 offsets: A=%u B=%u C=%u\n",
               (unsigned)P.y, (unsigned)P.x, (unsigned)a_off, (unsigned)b_off, (unsigned)c_off);
    }
    flex_global_barrier_xy();

    /* --- Leaders load their strips from HBM once --- */
    if (core == 0u) {
        /* Row leader loads A strip with 1D DMA (contiguous block) */
        if (P.x == 0u) {
            const uint32_t r = P.y; /* 0..3 row index */
            const uint32_t offA = hbm_off_A_strip(r);
            printf("[Load][A] C[%u,%u] row-leader loads A-strip r=%u from HBM off=0x%08x bytes=%u\n",
                   (unsigned)P.y, (unsigned)P.x, (unsigned)r, (unsigned)offA, (unsigned)BYTES_A_STRIP);
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
            printf("[Load][A] checksum=0x%08x\n", (unsigned)checksum_u32(addr_a, BYTES_A_STRIP));
        }

        /* Column leader loads B strip with 2D DMA (gather column-block) */
        if (P.y == 0u) {
            const uint32_t c = P.x; /* 0..3 col index */
            const uint32_t offB_base = hbm_off_B_strip_base(c);
            const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * ELEM_BYTES;    /* 64*4 */
            const uint32_t dst_stride   = size_per_row;                            /* packed in L1 */
            const uint32_t src_stride   = (uint32_t)MAT_N * ELEM_BYTES;            /* row-major in HBM */
            const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                  /* 256 rows */
            printf("[Load][B] C[%u,%u] col-leader loads B-strip c=%u from HBM base=0x%08x bytes/row=%u repeat=%u\n",
                   (unsigned)P.y, (unsigned)P.x, (unsigned)c, (unsigned)offB_base,
                   (unsigned)size_per_row, (unsigned)repeat);
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
            printf("[Load][B] checksum=0x%08x\n", (unsigned)checksum_u32(addr_b, BYTES_B_STRIP));
        }
    }
    flex_global_barrier_xy();

    /* --- Leaders broadcast strips across row (A) and column (B) --- */
    if (core == 0u) {
        if (P.x == 0u) {
            const uint16_t row_m = mask_row(P.y);  /* select this row */
            const uint16_t col_m = mask_all4();    /* all columns */
            printf("[Bcast][A] from C[%u,0] -> row %u, bytes=%u (dst_off=%u, src_off=%u)\n",
                   (unsigned)P.y, (unsigned)P.y, (unsigned)BYTES_A_STRIP, (unsigned)a_off, (unsigned)a_off);
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
        }
        if (P.y == 0u) {
            const uint16_t row_m = mask_all4();    /* all rows */
            const uint16_t col_m = mask_col(P.x);  /* select this column */
            printf("[Bcast][B] from C[0,%u] -> col %u, bytes=%u (dst_off=%u, src_off=%u)\n",
                   (unsigned)P.x, (unsigned)P.x, (unsigned)BYTES_B_STRIP, (unsigned)b_off, (unsigned)b_off);
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
        }
        flex_dma_async_wait_all();
    }
    flex_global_barrier_xy();

    /* --- Compute C tile and store to HBM --- */
    if (core == 0u) {
        printf("[Compute] C[%u,%u] matmul...\n", (unsigned)P.y, (unsigned)P.x);
        matmul_tile_fp32((const float*)addr_a, (const float*)addr_b, (float*)addr_c);
        printf("[Compute] done. checksum(C)=0x%08x\n", (unsigned)checksum_u32(addr_c, BYTES_C_TILE));

        /* Store C tile via 2D to its place in HBM */
        const uint32_t offC = hbm_off_C_tile(P.y, P.x);
        const uint32_t size_row = (uint32_t)C_TILE_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_str  = (uint32_t)MAT_N * ELEM_BYTES;       /* 256*4 */
        const uint32_t src_str  = size_row;                           /* packed in L1 */
        const uint32_t reps     = (uint32_t)C_TILE_ROWS;              /* 64      */
        printf("[Store] C[%u,%u] -> HBM off=0x%08x, bytes/row=%u, reps=%u\n",
               (unsigned)P.y, (unsigned)P.x, (unsigned)offC, (unsigned)size_row, (unsigned)reps);
        bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                          size_row, dst_str, src_str, reps);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    if (cid == 0u && core == 0u) {
        printf("[Done] All 16 tiles of C written to HBM at base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
