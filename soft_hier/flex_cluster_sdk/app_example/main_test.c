#include "flex_runtime.h"
#include "flex_printf.h"
#include "fixed_proj.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include <stdint.h>

/* ---------------- Additive checksum over 32-bit words ---------------- */
static inline uint64_t addsum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint32_t n = n_bytes >> 2;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += (uint64_t)p[i];
    return s;
}

/* ---------------- Simple HBM initialiser used by BOTH tests ----------- */
/* A: row-major 256x256 ramp (non-zero per strip)
 * B: row-major 256x256 identity, but columns {0,64,128,192} have 2.0f on diag
 * This guarantees every tile produces a non-zero sum.
 */
static void init_hbm_AB_for_fairness(void)
{
    /* A base and B base are contiguous windows in HBM (row-major) */
    volatile float *A = (volatile float *)hbm_addr(HBM_A_BASE_OFFSET);
    volatile float *B = (volatile float *)hbm_addr(HBM_B_BASE_OFFSET);

    /* A[i,j] = (i*256 + j + 1) * 1/65536 to keep sums modest and non-zero */
    for (uint32_t i = 0; i < MAT_N; ++i) {
        for (uint32_t j = 0; j < MAT_N; ++j) {
            uint32_t idx = i * MAT_N + j;
            float val = (float)(idx + 1u) * (1.0f / 65536.0f);
            A[idx] = val;
        }
    }

    /* B = identity, with 2.0f on columns {0,64,128,192} otherwise 1.0f on diag */
    for (uint32_t i = 0; i < MAT_N; ++i) {
        for (uint32_t j = 0; j < MAT_N; ++j) {
            float val = 0.0f;
            if (i == j) {
                if ((j % C_TILE_COLS) == 0u) val = 2.0f; /* 0,64,128,192 */
                else                           val = 1.0f;
            }
            B[i * MAT_N + j] = val;
        }
    }

    printf("[InitHBM] A: ramp; B: identity (2.0 on columns {0,64,128,192})\n");
}

/* ---------------- Naive fp32 GEMM: 64x256 times 256x64 -> 64x64 -------- */
static void matmul_64x256_256x64(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < C_TILE_ROWS; ++i) {
        for (uint32_t j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < A_STRIP_COLS; ++k) {
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

    /* Initialize allocators (per-cluster by first core) */
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    if (cid == 0 && core == 0) {
        printf("[Info] 4x4 cluster GEMM with inter-cluster broadcast (FP32)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem_bytes=%u\n", MAT_N, C_TILE_ROWS, ELEM_BYTES);
        printf("       A-strip bytes=%u, B-strip bytes=%u, C-tile bytes=%u\n",
               BYTES_A_STRIP, BYTES_B_STRIP, BYTES_C_TILE);
    }
    flex_global_barrier_xy();

    /* ---- One-time HBM initialisation for fairness ---- */
    if (cid == 0 && core == 0) {
        init_hbm_AB_for_fairness();
    }
    flex_global_barrier_xy();

    /* ---- Allocate L1 buffers on EVERY cluster (same sizes, same order) ---- */
    void *addr_a = 0;  /* 64x256 */
    void *addr_b = 0;  /* 256x64 */
    void *addr_c = 0;  /* 64x64  */

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
        printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u C=%u\n",
               P.y, P.x, (unsigned)a_off, (unsigned)b_off, (unsigned)c_off);
    }
    flex_global_barrier_xy();

    /* ---- Leaders pull their strips from HBM ---- */
    if (core == 0) {
        /* Row leader: x==0 owns A_r (horizontal strip for row r=y). */
        if (P.x == 0u) {
            const uint32_t r = P.y; /* 0..3 */
            const uint32_t hbm_off = hbm_off_A_strip(r);
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(hbm_off), BYTES_A_STRIP);
            bare_dma_wait_all();
            if (flex_is_dm_core()) {
                uint64_t sa = addsum_u32((void *)(addr_a), BYTES_A_STRIP);
                printf("[Load][A] C[%u,%u](DM) off=0x%08x bytes=%u | add=0x%016llx\n",
                       P.y, P.x, (unsigned)hbm_off, (unsigned)BYTES_A_STRIP,
                       (unsigned long long)sa);
            }
        }

        /* Column leader: y==0 owns B_c (vertical strip for col c=x). */
        if (P.y == 0u) {
            const uint32_t c = P.x; /* 0..3 */
            const uint32_t hbm_off = hbm_off_B_strip_base(c);
            const size_t size_per_row = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t dst_stride   = (size_t)B_STRIP_COLS * ELEM_BYTES;   /* 64*4 */
            const size_t src_stride   = (size_t)MAT_N        * ELEM_BYTES;   /* 256*4 */
            const size_t repeat       = (size_t)B_STRIP_ROWS;                /* 256   */
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(hbm_off),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
            if (flex_is_dm_core()) {
                uint64_t sb = addsum_u32((void *)(addr_b), BYTES_B_STRIP);
                printf("[Load][B] C[%u,%u](DM) base=0x%08x rows=%u | add=0x%016llx\n",
                       P.y, P.x, (unsigned)hbm_off, (unsigned)B_STRIP_ROWS,
                       (unsigned long long)sb);
            }
        }
    }
    flex_global_barrier_xy();

    /* ---- Inter-cluster broadcasts ---- */
    if (core == 0) {
        /* Horizontal broadcast of A_r from (x=0,y=r) to all x in that row. */
        if (P.x == 0u) {
            const uint16_t row_m = mask_row(P.y);   /* select this row */
            const uint16_t col_m = mask_all4();     /* all columns */
            if (flex_is_dm_core()) {
                printf("[Bcast][A] C[%u,%u](DM) -> row %u, bytes=%u (off=%u)\n",
                       P.y, P.x, P.y, (unsigned)BYTES_A_STRIP, (unsigned)a_off);
            }
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
        }

        /* Vertical broadcast of B_c from (x=c,y=0) to all y in that column. */
        if (P.y == 0u) {
            const uint16_t row_m = mask_all4();     /* all rows */
            const uint16_t col_m = mask_col(P.x);   /* select this column */
            if (flex_is_dm_core()) {
                printf("[Bcast][B] C[%u,%u](DM) -> col %u, bytes=%u (off=%u)\n",
                       P.y, P.x, P.x, (unsigned)BYTES_B_STRIP, (unsigned)b_off);
            }
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
        }

        flex_dma_async_wait_all();
    }
    flex_global_barrier_xy();

    /* Quick sanity after broadcast (DM cores only) */
    if (core == ARCH_NUM_CORE_PER_CLUSTER - 1) {
        uint64_t sa = addsum_u32(addr_a, BYTES_A_STRIP);
        uint64_t sb = addsum_u32(addr_b, BYTES_B_STRIP);
        printf("[Verify][AfterBcast] C[%u,%u]  A:add=0x%016llx  |  B:add=0x%016llx\n",
               P.y, P.x, (unsigned long long)sa, (unsigned long long)sb);
    }
    flex_global_barrier_xy();

    /* ---- Compute C_tile on core 0 of each cluster ---- */
    if (core == 0) {
        float *A = (float *)(addr_a);
        float *B = (float *)(addr_b);
        float *C = (float *)(addr_c);
        if (flex_is_dm_core()) printf("[Compute] C[%u,%u](DM) matmul...\n", P.y, P.x);
        matmul_64x256_256x64(A, B, C);
        uint64_t sc = addsum_u32(C, BYTES_C_TILE);
        printf("[Compute] C[%u,%u] done. add=0x%016llx\n",
               P.y, P.x, (unsigned long long)sc);
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
        if (flex_is_dm_core()) {
            printf("[Store]   C[%u,%u](DM) -> HBM off=0x%08x, bytes/row=%u, reps=%u\n",
                   P.y, P.x, (unsigned)hbm_off_c, (unsigned)size_row, (unsigned)reps);
        }
    }
    flex_global_barrier_xy();

    if (cid == 0 && core == 0) {
        printf("[Done] All 16 tiles of C written to HBM at base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
