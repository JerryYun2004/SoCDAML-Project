#include <stdint.h>
#include "flex_printf.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* -------- GEMM kernel: C(64x64) = A(64x256) * B(256x64) -------- */
static void matmul_tile_fp32(const float *A, const float *B, float *C)
{
    uint32_t i, j, k;
    for (i = 0; i < C_TILE_ROWS; ++i) {
        for (j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (k = 0; k < A_STRIP_COLS; ++k) { /* 256 */
                acc += A[i * A_STRIP_COLS + (uint32_t)k] *
                       B[(uint32_t)k * B_STRIP_COLS + j];
            }
            C[i * C_TILE_COLS + j] = acc;
        }
    }
}

/* ---- Initialize full A (256x256) and B (256x256) in HBM (once on C[0,0] DM) ---- */
static void init_hbm_A_and_B_once(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) {
        return;
    }

    const uint32_t ALIGN_DMA   = 64u;
    const uint32_t ROW_BYTES   = MAT_N * ELEM_BYTES; /* 256*4 = 1024 */
    void *rowbuf_raw = flex_l1_malloc(ROW_BYTES + ALIGN_DMA);
    if (rowbuf_raw == (void*)0) {
        printf("[InitHBM] ERROR: L1 row buffer alloc failed\n");
        flex_eoc(1);
        return;
    }
    void *rowbuf     = align_up_ptr(rowbuf_raw, ALIGN_DMA);
    uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, rowbuf);

    /* Fill A rows */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            /* Non-zero, deterministic pattern */
            pf[c] = (float)(r + 1u) * 0.001f + (float)(c + 1u) * 0.002f;
        }
        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        bare_dma_start_1d(/*dst*/ hbm_addr(offA_row), /*src*/ local(row_off), ROW_BYTES);
        bare_dma_wait_all();
    }

    /* Fill B rows */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            pf[c] = (float)(r + 1u) * 0.003f + (float)(c + 1u) * 0.004f;
        }
        const uint32_t offB_row = HBM_B_BASE_OFFSET + r * ROW_BYTES;
        bare_dma_start_1d(/*dst*/ hbm_addr(offB_row), /*src*/ local(row_off), ROW_BYTES);
        bare_dma_wait_all();
    }

    printf("[InitHBM] A and B initialized in HBM\n");
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
    const uint32_t IS_DM = flex_is_dm_core(); /* DM core == last core per cluster */

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
    flex_global_barrier_xy();

    /* --- Initialize A and B in HBM once on C[0,0] DM --- */
    init_hbm_A_and_B_once();
    flex_global_barrier_xy();

    /* ===========================================================
     * Allocate L1 on DM core only. Over-allocate and 64-align.
     * =========================================================== */
    void *addr_a_raw = (void*)0, *addr_b_raw = (void*)0, *addr_c_raw = (void*)0;
    void *addr_a     = (void*)0, *addr_b     = (void*)0, *addr_c     = (void*)0;
    uint32_t a_off   = 0u, b_off = 0u, c_off = 0u;
    const uint32_t   ALIGN_DMA = 64u;  /* safe alignment for iDMA */

    if (IS_DM) {
        addr_a_raw = flex_l1_malloc(BYTES_A_STRIP + ALIGN_DMA);
        addr_b_raw = flex_l1_malloc(BYTES_B_STRIP + ALIGN_DMA);
        addr_c_raw = flex_l1_malloc(BYTES_C_TILE  + ALIGN_DMA);

        if (addr_a_raw == (void*)0 || addr_b_raw == (void*)0 || addr_c_raw == (void*)0) {
            printf("[Error] L1 allocation failed (A=%p, B=%p, C=%p)\n",
                   addr_a_raw, addr_b_raw, addr_c_raw);
            flex_eoc(1); return 1;
        }

        /* 64-align user pointers for DMA destinations/sources */
        addr_a = align_up_ptr(addr_a_raw, ALIGN_DMA);
        addr_b = align_up_ptr(addr_b_raw, ALIGN_DMA);
        addr_c = align_up_ptr(addr_c_raw, ALIGN_DMA);

        /* Compute TCDM offsets from aligned pointers (for collectives & DMA) */
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);

        /* Clear C tile */
        zero_f32(addr_c, BYTES_C_TILE);
    }
    flex_global_barrier_xy();

    /* Ordered print of L1 offsets (row-major C[y,x]) — DM cores only */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u C=%u\n",
                       (unsigned)P.y, (unsigned)P.x,
                       (unsigned)a_off, (unsigned)b_off, (unsigned)c_off);
            }
        }
    }
    flex_global_barrier_xy();

    /* ==================== Load leaders (DM), ordered ==================== */

    /* Row leaders load A (contiguous 1D) */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        flex_global_barrier_xy();
        if (IS_DM && P.x == 0u && P.y == ry) {
            const uint32_t offA = hbm_off_A_strip(ry);
            printf("[Load][A] C[%u,0](DM) loads A-strip r=%u from HBM off=0x%08x bytes=%u\n",
                   (unsigned)ry, (unsigned)ry, (unsigned)offA, (unsigned)BYTES_A_STRIP);
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
            printf("[Load][A] checksum=0x%08x\n", (unsigned)checksum_u32(addr_a, BYTES_A_STRIP));
        }
    }
    flex_global_barrier_xy();

    /* Column leaders load B (2D gather of column-block from row-major HBM) */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (IS_DM && P.y == 0u && P.x == rx) {
            const uint32_t offB_base     = hbm_off_B_strip_base(rx);
            const uint32_t size_per_row  = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
            const uint32_t dst_stride    = size_per_row;                         /* packed L1 */
            const uint32_t src_stride    = (uint32_t)MAT_N * ELEM_BYTES;         /* 256*4 */
            const uint32_t repeat        = (uint32_t)B_STRIP_ROWS;               /* 256 rows */
            printf("[Load][B] C[0,%u](DM) loads B-strip c=%u from HBM base=0x%08x bytes/row=%u repeat=%u\n",
                   (unsigned)rx, (unsigned)rx, (unsigned)offB_base,
                   (unsigned)size_per_row, (unsigned)repeat);
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
            printf("[Load][B] checksum=0x%08x\n", (unsigned)checksum_u32(addr_b, BYTES_B_STRIP));
        }
    }
    flex_global_barrier_xy();

    /* ==================== Broadcasts (DM), ordered ==================== */

    /* A horizontally, row by row */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        flex_global_barrier_xy();
        if (IS_DM && P.x == 0u && P.y == ry) {
            const uint16_t row_m = mask_row(ry);
            const uint16_t col_m = mask_all4();
            printf("[Bcast][A] C[%u,0](DM) -> row %u, bytes=%u (off=%u)\n",
                   (unsigned)ry, (unsigned)ry, (unsigned)BYTES_A_STRIP, (unsigned)a_off);
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
            flex_dma_async_wait_all();
        }
    }
    flex_global_barrier_xy();

    /* B vertically, column by column */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (IS_DM && P.y == 0u && P.x == rx) {
            const uint16_t row_m = mask_all4();
            const uint16_t col_m = mask_col(rx);
            printf("[Bcast][B] C[0,%u](DM) -> col %u, bytes=%u (off=%u)\n",
                   (unsigned)rx, (unsigned)rx, (unsigned)BYTES_B_STRIP, (unsigned)b_off);
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
            flex_dma_async_wait_all();
        }
    }
    flex_global_barrier_xy();

    /* ==================== Compute & Store (DM), ordered ==================== */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                float *A = (float*)(uintptr_t)local(a_off);
                float *B = (float*)(uintptr_t)local(b_off);
                float *C = (float*)(uintptr_t)local(c_off);

                printf("[Compute] C[%u,%u](DM) matmul...\n", (unsigned)ry, (unsigned)rx);
                matmul_tile_fp32((const float*)A, (const float*)B, (float*)C);
                printf("[Compute] C[%u,%u] done. checksum=0x%08x\n",
                       (unsigned)ry, (unsigned)rx, (unsigned)checksum_u32(C, BYTES_C_TILE));

                const uint32_t offC     = hbm_off_C_tile(ry, rx);
                const uint32_t size_row = (uint32_t)C_TILE_COLS * ELEM_BYTES; /* 64*4 */
                const uint32_t dst_str  = (uint32_t)MAT_N * ELEM_BYTES;       /* 256*4 */
                const uint32_t src_str  = size_row;                           /* packed */
                const uint32_t reps     = (uint32_t)C_TILE_ROWS;              /* 64    */
                printf("[Store]   C[%u,%u](DM) -> HBM off=0x%08x, bytes/row=%u, reps=%u\n",
                       (unsigned)ry, (unsigned)rx, (unsigned)offC,
                       (unsigned)size_row, (unsigned)reps);
                bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                                  size_row, dst_str, src_str, reps);
                bare_dma_wait_all();
            }
        }
    }
    flex_global_barrier_xy();

    if (cid == 0u && core == 0u) {
        printf("[Done] All 16 tiles of C written to HBM at base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
