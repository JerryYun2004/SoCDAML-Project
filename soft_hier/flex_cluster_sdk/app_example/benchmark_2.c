/* benchmark_2.c — 1x4 clusters, 3D multiply with per-cluster B c-tiles (no broadcast)
 *
 * Tensors (FP32):
 *   A[a,b,t]        : 16 x 16 x 64  (shared; every worker loads full A)
 *   B[b,t,c]        : 16 x 64 x 64  (full in HBM, split into 4 tiles along c: 16 each)
 *   C[a,b,c]        : 16 x 16 x 64  (each worker writes its c-tile: 16)
 *
 * Worker set: row y==0, columns x==0..3 (DM core only). Others idle but synchronized.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"

/* ----------------- problem dims ----------------- */
enum {
    A_DIM_A = 16u,
    A_DIM_B = 16u,
    T_DIM   = 64u,
    C_DIM_C = 64u,    /* full C (and B) has 64 in c-dimension */
    C_TILE  = 16u     /* per-cluster c-tile size (4 tiles across x=0..3) */
};

#ifndef ELEM_BYTES
#define ELEM_BYTES 4u /* FP32 */
#endif

/* sizes */
#define A_ELEMS   (A_DIM_A * A_DIM_B * T_DIM)
#define B_ELEMS   (A_DIM_B * T_DIM   * C_DIM_C)    /* full B in HBM */
#define C_ELEMS   (A_DIM_A * A_DIM_B * C_DIM_C)    /* full C in HBM */
#define A_BYTES   (A_ELEMS * ELEM_BYTES)           /*  64 KiB */
#define B_BYTES   (B_ELEMS * ELEM_BYTES)           /* 256 KiB */
#define C_BYTES   (C_ELEMS * ELEM_BYTES)           /*  64 KiB */

#define B_TILE_ROWS   (A_DIM_B * T_DIM)            /* 16*64 = 1024 rows */
#define B_TILE_BYTES  (B_TILE_ROWS * C_TILE * ELEM_BYTES)   /* 1024 * 16 * 4 = 64 KiB */
#define C_TILE_ROWS   (A_DIM_A * A_DIM_B)          /* 16*16 = 256 rows */
#define C_TILE_BYTES  (C_TILE_ROWS * C_TILE * ELEM_BYTES)   /* 256 * 16 * 4 = 16 KiB */

/* ----------------- helpers ----------------- */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void*)v;
}
static inline void zero32(void *dst, uint32_t nbytes)
{
    volatile uint32_t *p = (volatile uint32_t*)dst;
    for (uint32_t i = 0; i < (nbytes >> 2); ++i) p[i] = 0u;
}
static inline uint64_t addsum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint32_t n = n_bytes >> 2;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += (uint64_t)p[i];
    return s;
}

/* 1D DMA helpers (blocking) */
static inline void dma_write_1d_to_hbm(uint32_t hbm_off, uint32_t l1_off, uint32_t bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off), /*src*/ local(l1_off), bytes);
    bare_dma_wait_all();
}
static inline void dma_read_1d_from_hbm(uint32_t l1_off, uint32_t hbm_off, uint32_t bytes)
{
    bare_dma_start_1d(/*dst*/ local(l1_off), /*src*/ hbm_addr(hbm_off), bytes);
    bare_dma_wait_all();
}

/* init patterns */
static void fill_A_pattern(float *A)
{
    for (uint32_t a = 0; a < A_DIM_A; ++a)
    for (uint32_t b = 0; b < A_DIM_B; ++b)
    for (uint32_t t = 0; t < T_DIM;   ++t) {
        const uint32_t ia = ((a * A_DIM_B) + b) * T_DIM + t;
        A[ia] = 1.0f + 0.001f*(float)a + 0.01f*(float)b + 0.0001f*(float)t;
    }
}
static void fill_B_full_pattern(float *B)
{
    for (uint32_t b = 0; b < A_DIM_B; ++b)
    for (uint32_t t = 0; t < T_DIM;   ++t)
    for (uint32_t c = 0; c < C_DIM_C; ++c) {
        const uint32_t ib = ((b * T_DIM) + t) * C_DIM_C + c;
        B[ib] = 0.5f + 0.02f*(float)b + 0.0002f*(float)t + 0.003f*(float)c;
    }
}

/* compute C_tile[a,b,c’] = sum_t A[a,b,t] * B_tile[b,t,c’] */
static void compute_3d_ctile(const float *A, const float *Btile, float *Ctile)
{
    for (uint32_t a = 0; a < A_DIM_A; ++a) {
        for (uint32_t b = 0; b < A_DIM_B; ++b) {
            for (uint32_t c = 0; c < C_TILE; ++c) {
                float acc = 0.0f;
                for (uint32_t t = 0; t < T_DIM; ++t) {
                    const uint32_t ia = ((a * A_DIM_B) + b) * T_DIM + t;
                    const uint32_t ib = ((b * T_DIM) + t) * C_TILE + c; /* tile has last dim = C_TILE */
                    acc += A[ia] * Btile[ib];
                }
                const uint32_t ic = ((a * A_DIM_B) + b) * C_TILE + c;
                Ctile[ic] = acc;
            }
        }
    }
}

/* ----------------- main ----------------- */
int main(void)
{
    /* Bring-up (every core) */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t IS_DM = flex_is_dm_core();

    /* Worker = DM cores in row y==0, columns x==0..3 */
    const uint32_t DO_WORK = (uint32_t)(IS_DM && (P.y == 0u) && (P.x < 4u));
    const uint32_t TILE_X  = P.x;                         /* which c-tile (0..3) */
    const uint32_t C0      = (uint32_t)(TILE_X * C_TILE); /* starting c-index for this tile */

    if (cid == 0u && core == 0u) {
        printf("[Info][Bmk2] 1x4 (row 0) clusters, no broadcast; each pulls A + its B c-tile\n");
        printf("       A=%ux%ux%u bytes=%u\n", (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)T_DIM,   (unsigned)A_BYTES);
        printf("       B_full=%ux%ux%u bytes=%u, tiles along c: 4 x (..x%u)\n",
               (unsigned)A_DIM_B, (unsigned)T_DIM, (unsigned)C_DIM_C, (unsigned)B_BYTES, (unsigned)C_TILE);
        printf("       C_full=%ux%ux%u bytes=%u, per tile bytes=%u\n",
               (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)C_DIM_C, (unsigned)C_BYTES, (unsigned)C_TILE_BYTES);
    }
    flex_global_barrier_xy();

    /* =================== Phase 1: C(0,0) DM initializes A and B_full in HBM =================== */
    if (IS_DM && P.x == 0u && P.y == 0u) {
        void *raw_a = flex_l1_malloc(A_BYTES + 64u);
        void *raw_b = flex_l1_malloc(B_BYTES + 64u);
        if (!raw_a || !raw_b) {
            printf("[ERR] L1 alloc failed for init: A=%p B=%p (C00 DM)\n", raw_a, raw_b);
        } else {
            void *aln_a = align64(raw_a);
            void *aln_b = align64(raw_b);
            uint32_t off_a = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_a);
            uint32_t off_b = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_b);
            float *A = (float*)(uintptr_t)local(off_a);
            float *B = (float*)(uintptr_t)local(off_b);

            fill_A_pattern(A);
            fill_B_full_pattern(B);

            printf("[HBM][Write] A -> 0x%08x (%u B)\n", (unsigned)HBM_A_BASE_OFFSET, (unsigned)A_BYTES);
            dma_write_1d_to_hbm((uint32_t)HBM_A_BASE_OFFSET, off_a, A_BYTES);

            printf("[HBM][Write] B_full -> 0x%08x (%u B)\n", (unsigned)HBM_B_BASE_OFFSET, (unsigned)B_BYTES);
            dma_write_1d_to_hbm((uint32_t)HBM_B_BASE_OFFSET, off_b, B_BYTES);
        }
    }
    /* Everyone waits for A/B to be in HBM */
    flex_global_barrier_xy();

    /* =================== Phase 2: each worker loads A (1D) + B c-tile (2D) =================== */
    void *raw_A = 0, *raw_Bt = 0, *raw_Ct = 0;
    void *aln_A = 0, *aln_Bt = 0, *aln_Ct = 0;
    uint32_t off_A = 0, off_Bt = 0, off_Ct = 0;
    float *A = 0, *Btile = 0, *Ctile = 0;

    /* Keep a per-worker “ok” flag. Never call flex_eoc() mid-program; still join barriers. */
    uint32_t work_ok = 1u;

    if (DO_WORK) {
        printf("[C(0,%u) DM] Phase2: L1 alloc & HBM reads start\n", (unsigned)P.x);

        raw_A  = flex_l1_malloc(A_BYTES       + 64u);
        raw_Bt = flex_l1_malloc(B_TILE_BYTES  + 64u);
        raw_Ct = flex_l1_malloc(C_TILE_BYTES  + 64u);
        if (!raw_A || !raw_Bt || !raw_Ct) {
            printf("[ERR][C(0,%u) DM] L1 alloc failed: A=%p Btile=%p Ctile=%p\n",
                   (unsigned)P.x, raw_A, raw_Bt, raw_Ct);
            work_ok = 0u;
        } else {
            aln_A  = align64(raw_A);
            aln_Bt = align64(raw_Bt);
            aln_Ct = align64(raw_Ct);

            off_A  = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_A);
            off_Bt = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_Bt);
            off_Ct = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_Ct);

            A     = (float*)(uintptr_t)local(off_A);
            Btile = (float*)(uintptr_t)local(off_Bt);
            Ctile = (float*)(uintptr_t)local(off_Ct);

            zero32(Ctile, C_TILE_BYTES);

            /* Pull A (contiguous 64 KiB) */
            dma_read_1d_from_hbm(off_A, (uint32_t)HBM_A_BASE_OFFSET, A_BYTES);

            /* Pull B c-tile via 2D
             * rows      = B_TILE_ROWS = 1024
             * rowSize   = C_TILE * 4  = 16 * 4 = 64
             * srcStride = C_DIM_C * 4 = 64 * 4 = 256
             * dstStride = rowSize     = 64
             * srcBase   = HBM_B_BASE_OFFSET + (C0 * 4)
             *
             * Safety check: last byte read = srcBase + (rows-1)*srcStride + rowSize
             *   = base + 1023*256 + 64 = base + 261,952
             * For the highest tile (C0=48 -> +192), this is base + 262,144 == B_BYTES. OK.
             */
            const uint32_t src_base  = (uint32_t)HBM_B_BASE_OFFSET + C0 * ELEM_BYTES;
            const uint32_t rowSize   = (uint32_t)(C_TILE * ELEM_BYTES);   /* 64  */
            const uint32_t dstStride = rowSize;                           /* 64  */
            const uint32_t srcStride = (uint32_t)(C_DIM_C * ELEM_BYTES);  /* 256 */
            const uint32_t rows      = (uint32_t)B_TILE_ROWS;             /* 1024 */

            bare_dma_start_2d(/*dst*/ local(off_Bt), /*src*/ hbm_addr(src_base),
                              rowSize, dstStride, srcStride, rows);
            bare_dma_wait_all();

            /* quick checksums (lightweight) */
            uint64_t sa = addsum_u32(A,     A_BYTES);
            uint64_t sb = addsum_u32(Btile, B_TILE_BYTES);
            printf("[CHK][C(0,%u)] add(A)=0x%08x%08x  add(Btile)=0x%08x%08x\n",
                   (unsigned)P.x,
                   (unsigned)(sa >> 32), (unsigned)(sa & 0xFFFFFFFFu),
                   (unsigned)(sb >> 32), (unsigned)(sb & 0xFFFFFFFFu));
        }
    }
    /* Everyone syncs (workers & non-workers) */
    flex_global_barrier_xy();

    /* =================== Phase 3: compute per-cluster c-tile =================== */
    if (DO_WORK && work_ok) {
        printf("[C(0,%u) DM] Phase3: compute start\n", (unsigned)P.x);
        compute_3d_ctile(A, Btile, Ctile);
        uint64_t sc = addsum_u32(Ctile, C_TILE_BYTES);
        printf("[CHK][C(0,%u)] add(Ctile)=0x%08x%08x\n",
               (unsigned)P.x, (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));
    }
    flex_global_barrier_xy();

    /* =================== Phase 4: store C-tile back to HBM =================== */
    if (DO_WORK && work_ok) {
        /* 2D scatter to full C buffer, same row geometry as B tile:
         * rows      = C_TILE_ROWS = 256
         * rowSize   = C_TILE * 4  = 64
         * dstStride = C_DIM_C * 4 = 256
         * srcStride = rowSize     = 64
         * dstBase   = HBM_C_BASE_OFFSET + (C0 * 4)
         */
        const uint32_t dst_base  = (uint32_t)HBM_C_BASE_OFFSET + C0 * ELEM_BYTES;
        const uint32_t rowSize   = (uint32_t)(C_TILE * ELEM_BYTES);
        const uint32_t dstStride = (uint32_t)(C_DIM_C * ELEM_BYTES);
        const uint32_t srcStride = rowSize;
        const uint32_t rows      = (uint32_t)C_TILE_ROWS;

        bare_dma_start_2d(/*dst*/ hbm_addr(dst_base), /*src*/ local(off_Ct),
                          rowSize, dstStride, srcStride, rows);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    if (cid == 0u && core == 0u) {
        printf("[Done][Bmk2] All four C tiles stored to HBM base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
