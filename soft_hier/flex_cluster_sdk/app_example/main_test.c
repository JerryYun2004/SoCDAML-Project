#include <stdint.h>
#include "flex_printf.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* ---------------- Checksums ---------------- */
static inline uint64_t checksum_add_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t*)ptr;
    uint32_t n = n_bytes >> 2;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) { s += (uint64_t)p[i]; }
    return s;
}

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

/* --- Helper: write one row to HBM with 1D DMA from a 64B-aligned L1 rowbuf --- */
static void dma_write_row_to_hbm(uint32_t hbm_off_row, uint32_t row_off_l1, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(row_off_l1), row_bytes);
    bare_dma_wait_all();
}

/* ---- Initialize A (sentinel per 64x64 tile) and B (mostly identity) in HBM, once on C[0,0] DM ----
 *
 * A: zero everywhere EXCEPT for tile (r,c) we set A[r*64+13][c*64+17] to a *unique* 32b float pattern.
 *     This guarantees each C tile XOR = that unique pattern (non-zero) when B is identity-like.
 *
 * B: diagonal = 1.0f, EXCEPT for the first column of each 64-col block (j = 0,64,128,192) we set B[j][j] = 2.0f
 *     so that each B-strip XOR is also non-zero (good for leader load checks). This does not affect our sentinels.
 */
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
        uint32_t *p32 = (uint32_t*)(uintptr_t)local(row_off);
        /* zero row */
        for (uint32_t c = 0; c < MAT_N; ++c) { p32[c] = 0u; }

        /* If this row is the sentinel row of its 64-row tile, place 4 unique sentinels at columns c*64+17 */
        if ((r & 63u) == 13u) {
            uint32_t tile_r = r >> 6; /* r / 64 --> 0..3 */
            for (uint32_t tile_c = 0; tile_c < 4u; ++tile_c) {
                uint32_t col = tile_c * 64u + 17u;
                /* Unique, finite float bitpattern – vary by tile (r,c) and ensure row-strip XOR != 0 */
                uint32_t bits = 0x3F800001u /* ~1.0000001f */
                              ^ (1u << tile_c)   /* distinct per tile_c: 1,2,4,8 */
                              ^ (tile_r << 8);   /* distinct per tile_r in higher bits */
                p32[col] = bits;
            }
        }

        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row_to_hbm(offA_row, row_off, ROW_BYTES);
    }

    /* Fill B rows: identity except one 2.0f per 64-col block */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        uint32_t *p32 = (uint32_t*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) { p32[c] = 0u; }

        /* diagonal element at (r,r) */
        uint32_t j = r;  /* diagonal column equals row index */
        uint32_t block_start = (j >> 6) << 6;  /* 0, 64, 128, 192 */
        /* put 2.0f (0x40000000) at the first column of the block; else 1.0f (0x3f800000) */
        p32[j] = (j == block_start) ? 0x40000000u : 0x3F800000u;

        const uint32_t offB_row = HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row_to_hbm(offB_row, row_off, ROW_BYTES);
    }

    printf("[InitHBM] A: sentinel-per-tile; B: identity w/ 2.0 on columns {0,64,128,192}\n");
}

/* Enable to read back and verify first row of each stored C tile (off by default) */
#define VERIFY_STORE_READBACK 0

int main(void)
{
    /* --- Bring-up and global sync --- */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    flex_alloc_init();  /* allocator prints */

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
    const uint32_t   ALIGN_DMA = 64u;

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

        /* TCDM offsets from aligned pointers (for collectives & DMA) */
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);

        /* Clear C tile */
        zero_f32(addr_c, BYTES_C_TILE);
    }
    flex_global_barrier_xy();

    /* Ordered print of L1 offsets — DM cores only */
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
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
            /* Expect non-zero due to 4 distinct sentinels in this 64-row band */
            uint32_t ax = checksum_u32(addr_a, BYTES_A_STRIP);
            printf("[Load][A] C[%u,0](DM) off=0x%08x bytes=%u | xor=0x%08x add=0x%08x%08x\n",
                   (unsigned)ry, (unsigned)offA, (unsigned)BYTES_A_STRIP,
                   (unsigned)ax,
                   (unsigned)(checksum_add_u32(addr_a, BYTES_A_STRIP) >> 32),
                   (unsigned)(checksum_add_u32(addr_a, BYTES_A_STRIP) & 0xFFFFFFFFu));
        }
    }
    flex_global_barrier_xy();

    /* Column leaders load B (2D gather from row-major HBM) */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (IS_DM && P.y == 0u && P.x == rx) {
            const uint32_t offB_base     = hbm_off_B_strip_base(rx);
            const uint32_t size_per_row  = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
            const uint32_t dst_stride    = size_per_row;                         /* packed L1 */
            const uint32_t src_stride    = (uint32_t)MAT_N * ELEM_BYTES;         /* 256*4 */
            const uint32_t repeat        = (uint32_t)B_STRIP_ROWS;               /* 256 rows */
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
            /* Expect non-zero due to 2.0f at the first column of each 64-col block */
            uint32_t bx = checksum_u32(addr_b, BYTES_B_STRIP);
            printf("[Load][B] C[0,%u](DM) base=0x%08x rows=%u | xor=0x%08x add=0x%08x%08x\n",
                   (unsigned)rx, (unsigned)offB_base, (unsigned)repeat,
                   (unsigned)bx,
                   (unsigned)(checksum_add_u32(addr_b, BYTES_B_STRIP) >> 32),
                   (unsigned)(checksum_add_u32(addr_b, BYTES_B_STRIP) & 0xFFFFFFFFu));
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
            const uint16_t row_m = mask_all_rows();
            const uint16_t col_m = mask_col(rx);
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
            flex_dma_async_wait_all();
        }
    }
    flex_global_barrier_xy();

    /* Verify after broadcast (every DM core prints once in row-major) */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                uint32_t ax = checksum_u32((void*)(uintptr_t)local(a_off), BYTES_A_STRIP);
                uint32_t bx = checksum_u32((void*)(uintptr_t)local(b_off), BYTES_B_STRIP);
                uint64_t as = checksum_add_u32((void*)(uintptr_t)local(a_off), BYTES_A_STRIP);
                uint64_t bs = checksum_add_u32((void*)(uintptr_t)local(b_off), BYTES_B_STRIP);
                printf("[Verify][AfterBcast] C[%u,%u]  A: xor=0x%08x add=0x%08x%08x  |  B: xor=0x%08x add=0x%08x%08x\n",
                       (unsigned)ry,(unsigned)rx,
                       (unsigned)ax,(unsigned)(as>>32),(unsigned)as,
                       (unsigned)bx,(unsigned)(bs>>32),(unsigned)bs);
            }
        }
    }
    flex_global_barrier_xy();

    /* ==================== Compute & Store (DM), ordered ==================== */
    #if VERIFY_STORE_READBACK
    /* scratch row buffer for optional readback verify */
    uint32_t rb_off = 0;
    if (IS_DM) {
        void *rb_raw = flex_l1_malloc((uint32_t)C_TILE_COLS * ELEM_BYTES + 64u);
        void *rb     = align_up_ptr(rb_raw, 64u);
        rb_off       = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, rb);
    }
    #endif

    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                float *A = (float*)(uintptr_t)local(a_off);
                float *B = (float*)(uintptr_t)local(b_off);
                float *C = (float*)(uintptr_t)local(c_off);

                matmul_tile_fp32((const float*)A, (const float*)B, (float*)C);

                uint32_t cx = checksum_u32(C, BYTES_C_TILE);
                uint64_t cs = checksum_add_u32(C, BYTES_C_TILE);
                /* With our A/B init, this XOR is guaranteed non-zero for all tiles */
                printf("[Compute] C[%u,%u] done. xor=0x%08x add=0x%08x%08x\n",
                       (unsigned)ry,(unsigned)rx,
                       (unsigned)cx,(unsigned)(cs>>32),(unsigned)cs);

                const uint32_t offC     = hbm_off_C_tile(ry, rx);
                const uint32_t size_row = (uint32_t)C_TILE_COLS * ELEM_BYTES; /* 64*4 */
                const uint32_t dst_str  = (uint32_t)MAT_N * ELEM_BYTES;       /* 256*4 */
                const uint32_t src_str  = size_row;                           /* packed */
                const uint32_t reps     = (uint32_t)C_TILE_ROWS;              /* 64    */
                bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                                  size_row, dst_str, src_str, reps);
                bare_dma_wait_all();

                #if VERIFY_STORE_READBACK
                /* Read back first row of this tile from HBM and print its XOR */
                bare_dma_start_1d(/*dst*/ local(rb_off), /*src*/ hbm_addr(offC), size_row);
                bare_dma_wait_all();
                uint32_t rbx = checksum_u32((void*)(uintptr_t)local(rb_off), size_row);
                printf("[Verify][StoreRB] C[%u,%u] first-row xor=0x%08x\n",
                       (unsigned)ry,(unsigned)rx,(unsigned)rbx);
                #endif
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
