/* benchmark_2.c — 1x4 clusters, 3D multiply with per-cluster B c-tiles (no broadcast)
 *
 * Tensors (FP32):
 *   A[a,b,t]        : 16 x 16 x 64  (shared; every worker cluster loads full A)
 *   B[b,t,c]        : 16 x 64 x 64  (full in HBM, split into 4 tiles along c: 16 each)
 *   C[a,b,c]        : 16 x 16 x 64  (each worker writes its c-tile: 16)
 *
 * Worker set: row y==0, columns x==0..3 (DM core only). Others idle but synchronized.
 *
 * Layout (row-major; last index fastest):
 *   A idx: ((a * A_DIM_B) + b) * T_DIM + t
 *   B idx: ((b * T_DIM) + t) * C_DIM_C + c
 *   C idx: ((a * A_DIM_B) + b) * C_DIM_C + c
 *
 * DMA:
 *   - A: single 1D transfer of A_BYTES.
 *   - B tile: 2D gather with rowSize = C_TILE * 4, rows = (A_DIM_B*T_DIM),
 *             srcStride = C_DIM_C * 4, dstStride = rowSize.
 *   - C tile store: 2D scatter with same row geometry as B tile.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"     /* HBM_*_BASE_OFFSET, local(), hbm_addr(), barriers, etc. */
#include "flex_printf.h"

/* ----------------- problem dims ----------------- */
enum {
    A_DIM_A = 16u,
    A_DIM_B = 16u,
    T_DIM   = 64u,
    C_DIM_C = 64u,    /* full C (and B) has 64 in c-dimension */
    C_TILE  = 16u     /* per-cluster c-tile size (4 tiles across x=0..3) */
};

/* element size (fp32) — typically 4; keep explicit for clarity */
#ifndef ELEM_BYTES
#define ELEM_BYTES 4u
#endif

/* sizes */
#define A_ELEMS   (A_DIM_A * A_DIM_B * T_DIM)
#define B_ELEMS   (A_DIM_B * T_DIM   * C_DIM_C)    /* full B in HBM */
#define C_ELEMS   (A_DIM_A * A_DIM_B * C_DIM_C)    /* full C in HBM */
#define A_BYTES   (A_ELEMS * ELEM_BYTES)
#define B_BYTES   (B_ELEMS * ELEM_BYTES)           /* full */
#define C_BYTES   (C_ELEMS * ELEM_BYTES)           /* full */

#define B_TILE_ROWS   (A_DIM_B * T_DIM)            /* 16*64 = 1024 rows */
#define B_TILE_BYTES  (B_TILE_ROWS * C_TILE * ELEM_BYTES)
#define C_TILE_ROWS   (A_DIM_A * A_DIM_B)          /* 16*16 = 256 rows */
#define C_TILE_BYTES  (C_TILE_ROWS * C_TILE * ELEM_BYTES)

/* ----------------- tiny helpers ----------------- */
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

/* ----------------- patterns ----------------- */
static void fill_A_pattern(float *A)
{
    /* A[a,b,t] = 1.0 + 0.001*a + 0.01*b + 0.0001*t */
    for (uint32_t a = 0; a < A_DIM_A; ++a)
    for (uint32_t b = 0; b < A_DIM_B; ++b)
    for (uint32_t t = 0; t < T_DIM;   ++t) {
        const uint32_t ia = ((a * A_DIM_B) + b) * T_DIM + t;
        A[ia] = 1.0f + 0.001f*(float)a + 0.01f*(float)b + 0.0001f*(float)t;
    }
}

static void fill_B_full_pattern(float *B)
{
    /* B[b,t,c] = 0.5 + 0.02*b + 0.0002*t + 0.003*c, for c in [0..63] */
    for (uint32_t b = 0; b < A_DIM_B; ++b)
    for (uint32_t t = 0; t < T_DIM;   ++t)
    for (uint32_t c = 0; c < C_DIM_C; ++c) {
        const uint32_t ib = ((b * T_DIM) + t) * C_DIM_C + c;
        B[ib] = 0.5f + 0.02f*(float)b + 0.0002f*(float)t + 0.003f*(float)c;
    }
}

/* ----------------- compute: c-tile ----------------- */
/* C_tile[a,b,c'] = sum_t A[a,b,t] * B_tile[b,t,c'] ,  where c' in [0..C_TILE-1] */
static void compute_3d_ctile(const float *A, const float *Btile, float *Ctile)
{
    for (uint32_t a = 0; a < A_DIM_A; ++a) {
        for (uint32_t b = 0; b < A_DIM_B; ++b) {
            for (uint32_t c = 0; c < C_TILE; ++c) {
                float acc = 0.0f;
                for (uint32_t t = 0; t < T_DIM; ++t) {
                    const uint32_t ia = ((a * A_DIM_B) + b) * T_DIM + t;
                    const uint32_t ib = ((b * T_DIM) + t) * C_TILE + c; /* B tile last dim = C_TILE */
                    acc += A[ia] * Btile[ib];
                }
                const uint32_t ic = ((a * A_DIM_B) + b) * C_TILE + c;   /* C tile */
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
    const uint32_t TILE_X  = P.x;                           /* which c-tile (0..3) */
    const uint32_t C0      = (uint32_t)(TILE_X * C_TILE);   /* starting c-index for this tile */

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
        /* L1 scratch big enough for A + B_full (done sequentially to minimize L1 pressure) */
        void *raw_a = flex_l1_malloc(A_BYTES + 64u);
        void *raw_b = flex_l1_malloc(B_BYTES + 64u);
        if (!raw_a || !raw_b) {
            printf("[ERR] L1 alloc failed for init: A=%p B=%p\n", raw_a, raw_b);
            flex_eoc(1); return 1;
        }
        void *aln_a = align64(raw_a);
        void *aln_b = align64(raw_b);
        uint32_t off_a = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_a);
        uint32_t off_b = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_b);
        float *A = (float*)(uintptr_t)local(off_a);
        float *B = (float*)(uintptr_t)local(off_b);

        /* fill patterns */
        fill_A_pattern(A);
        fill_B_full_pattern(B);

        /* write to HBM */
        printf("[HBM][Write] A -> 0x%08x (%u B)\n", (unsigned)HBM_A_BASE_OFFSET, (unsigned)A_BYTES);
        dma_write_1d_to_hbm((uint32_t)HBM_A_BASE_OFFSET, off_a, A_BYTES);

        printf("[HBM][Write] B_full -> 0x%08x (%u B)\n", (unsigned)HBM_B_BASE_OFFSET, (unsigned)B_BYTES);
        dma_write_1d_to_hbm((uint32_t)HBM_B_BASE_OFFSET, off_b, B_BYTES);
    }
    flex_global_barrier_xy();

    /* =================== Phase 2: each worker allocates L1, then HBM reads (serialized) =================== */
    void *raw_A = 0, *raw_Bt = 0, *raw_Ct = 0;
    void *aln_A = 0, *aln_Bt = 0, *aln_Ct = 0;
    uint32_t off_A = 0, off_Bt = 0, off_Ct = 0;
    float *A = 0, *Btile = 0, *Ctile = 0;

    if (DO_WORK) {
        printf("[C(0,%u) DM] Phase2: L1 alloc & HBM reads start\n", (unsigned)P.x);

        raw_A  = flex_l1_malloc(A_BYTES      + 64u);
        raw_Bt = flex_l1_malloc(B_TILE_BYTES + 64u);
        raw_Ct = flex_l1_malloc(C_TILE_BYTES + 64u);
        if (!raw_A || !raw_Bt || !raw_Ct) {
            printf("[ERR][C(0,%u) DM] L1 alloc failed: A=%p Btile=%p Ctile=%p\n",
                   (unsigned)P.x, raw_A, raw_Bt, raw_Ct);
            flex_eoc(1); return 1;
        }

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
    }
    flex_global_barrier_xy();

    /* ---- Serialize the HBM reads: only one worker (x==rx) pulls at a time ---- */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        /* Everyone arrives here; only C(0,rx) DM performs the reads. */
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            /* Pull A (contiguous 1D) */
            dma_read_1d_from_hbm(off_A, (uint32_t)HBM_A_BASE_OFFSET, A_BYTES);

            /* Pull B c-tile via 2D gather:
             *   rows      = B_TILE_ROWS = 16*64
             *   rowSize   = C_TILE * 4
             *   srcStride = C_DIM_C * 4
             *   dstStride = rowSize
             *   srcBase   = HBM_B_BASE_OFFSET + (rx*C_TILE)*4
             */
            const uint32_t C0_read  = rx * C_TILE;
            const uint32_t src_base = (uint32_t)HBM_B_BASE_OFFSET + C0_read * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(C_TILE * ELEM_BYTES);
            const uint32_t dstStr   = rowSize;
            const uint32_t srcStr   = (uint32_t)(C_DIM_C * ELEM_BYTES);
            const uint32_t rows     = (uint32_t)B_TILE_ROWS;

            bare_dma_start_2d(/*dst*/ local(off_Bt), /*src*/ hbm_addr(src_base),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();

            /* Quick checksums (sanity) */
            uint64_t sa = addsum_u32(A,     A_BYTES);
            uint64_t sb = addsum_u32(Btile, B_TILE_BYTES);
            printf("[CHK][C(0,%u)] add(A)=0x%08x%08x  add(Btile)=0x%08x%08x\n",
                   (unsigned)rx,
                   (unsigned)(sa >> 32), (unsigned)(sa & 0xFFFFFFFFu),
                   (unsigned)(sb >> 32), (unsigned)(sb & 0xFFFFFFFFu));
        }

        /* Everyone waits until that worker has finished reading. */
        flex_global_barrier_xy();
    }

    /* =================== Phase 3: compute per-cluster c-tile (parallel across 4 workers) =================== */
    if (DO_WORK) {
        compute_3d_ctile(A, Btile, Ctile);
        uint64_t sc = addsum_u32(Ctile, C_TILE_BYTES);
        printf("[CHK][C(0,%u)] add(Ctile)=0x%08x%08x\n",
               (unsigned)P.x, (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));
    }
    flex_global_barrier_xy();

    /* =================== Phase 4: store C-tile back to HBM (serialized, like reads) =================== */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            /* 2D scatter to full C buffer:
             *   rows      = C_TILE_ROWS = 16*16
             *   rowSize   = C_TILE * 4
             *   dstStride = C_DIM_C * 4
             *   srcStride = rowSize
             *   dstBase   = HBM_C_BASE_OFFSET + (rx*C_TILE)*4
             */
            const uint32_t dst_base = (uint32_t)HBM_C_BASE_OFFSET + (rx * C_TILE) * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(C_TILE * ELEM_BYTES);
            const uint32_t dstStr   = (uint32_t)(C_DIM_C * ELEM_BYTES);
            const uint32_t srcStr   = rowSize;
            const uint32_t rows     = (uint32_t)C_TILE_ROWS;

            bare_dma_start_2d(/*dst*/ hbm_addr(dst_base), /*src*/ local(off_Ct),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();
        }

        flex_global_barrier_xy();
    }

    if (cid == 0u && core == 0u) {
        printf("[Done][Bmk2] All four C tiles stored to HBM base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
