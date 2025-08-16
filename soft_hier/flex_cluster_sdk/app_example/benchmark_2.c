/* benchmark_2.c — 1x4 clusters, 2D GEMM with per-cluster B column tiles (no broadcast)
 *
 * GEMM (FP32):
 *   A[M,K]        : 64 x 256   (shared; every worker cluster loads full A)
 *   B[K,N]        : 256 x 256  (full in HBM, split into 4 column-tiles of 64 each)
 *   C[M,N]        : 64 x 256   (each worker writes its N-tile: 64 columns)
 *
 * Worker set: row y==0, columns x==0..3 (DM core only). Others idle but synchronized.
 *
 * Layout (row-major):
 *   A[i,k]  at i*K + k
 *   B[k,j]  at k*N + j
 *   C[i,j]  at i*N + j
 *
 * DMA:
 *   - A:      single 1D transfer of A_BYTES.
 *   - B tile: 2D gather: rows=K, rowSize=TILE_N*4, srcStride=N*4, dstStride=rowSize.
 *   - C tile: 2D scatter: rows=M, rowSize=TILE_N*4, dstStride=N*4, srcStride=rowSize.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"     /* HBM_*_BASE_OFFSET, local(), hbm_addr(), barriers, etc. */
#include "flex_printf.h"

/* ----------------- GEMM dims ----------------- */
enum {
    M_DIM   = 64u,    /* rows of A, C */
    K_DIM   = 256u,   /* cols of A / rows of B */
    N_DIM   = 256u,   /* cols of B, C */
    TILE_N  = 64u     /* per-cluster N-tile size (4 tiles across x=0..3) */
};

/* element size (fp32) — explicit for clarity */
#ifndef ELEM_BYTES
#define ELEM_BYTES 4u
#endif

/* sizes (elements / bytes) */
#define A_ELEMS   (M_DIM * K_DIM)          /* 64*256  = 16384  */
#define B_ELEMS   (K_DIM * N_DIM)          /* 256*256 = 65536  */
#define C_ELEMS   (M_DIM * N_DIM)          /* 64*256  = 16384  */
#define A_BYTES   (A_ELEMS * ELEM_BYTES)   /*  64 KB  */
#define B_BYTES   (B_ELEMS * ELEM_BYTES)   /* 256 KB  */
#define C_BYTES   (C_ELEMS * ELEM_BYTES)   /*  64 KB  */

/* 2D DMA geometry for tiles */
#define B_TILE_ROWS   (K_DIM)                           /* 256 rows */
#define B_TILE_BYTES  (B_TILE_ROWS * TILE_N * ELEM_BYTES)   /* 256*64*4 = 64 KB */
#define C_TILE_ROWS   (M_DIM)                           /* 64 rows */
#define C_TILE_BYTES  (C_TILE_ROWS * TILE_N * ELEM_BYTES)    /* 64*64*4  = 16 KB */

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
static void fill_A_pattern(float *A)   /* A[M,K], row-major */
{
    /* A[i,k] = 1.0 + 0.001*i + 0.0001*k */
    for (uint32_t i = 0; i < M_DIM; ++i)
    for (uint32_t k = 0; k < K_DIM; ++k) {
        const uint32_t ia = i * K_DIM + k;
        A[ia] = 1.0f + 0.001f*(float)i + 0.0001f*(float)k;
    }
}

static void fill_B_full_pattern(float *B)  /* B[K,N], row-major */
{
    /* B[k,j] = 0.5 + 0.002*k + 0.01*j */
    for (uint32_t k = 0; k < K_DIM; ++k)
    for (uint32_t j = 0; j < N_DIM; ++j) {
        const uint32_t ib = k * N_DIM + j;
        B[ib] = 0.5f + 0.002f*(float)k + 0.01f*(float)j;
    }
}

/* ----------------- compute: per-cluster N-tile ----------------- */
/* C_tile[M, TILE_N] = A[M,K] * B_tile[K, TILE_N] */
static void compute_gemm_tile(const float *A, const float *Btile, float *Ctile)
{
    for (uint32_t i = 0; i < M_DIM; ++i) {
        float *c_row = &Ctile[i * TILE_N];
        const float *a_row = &A[i * K_DIM];
        for (uint32_t j = 0; j < TILE_N; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < K_DIM; ++k) {
                acc += a_row[k] * Btile[k * TILE_N + j];
            }
            c_row[j] = acc;
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
    const uint32_t DO_WORK         = (uint32_t)(IS_DM && (P.y == 0u) && (P.x < 4u));
    const uint32_t TILE_X          = P.x;                       /* which N-tile (0..3) */
    const uint32_t N0              = (uint32_t)(TILE_X * TILE_N);   /* starting column index for this tile */
    const uint32_t is_timer_master = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u));

    if (cid == 0u && core == 0u) {
        printf("[Info][Bmk2-2D] 1x4 (row 0), no broadcast; each pulls A + its B N-tile\n");
        printf("       A=%ux%u bytes=%u\n", (unsigned)M_DIM, (unsigned)K_DIM, (unsigned)A_BYTES);
        printf("       B_full=%ux%u bytes=%u, tiles along N: 4 x (%u cols)\n",
               (unsigned)K_DIM, (unsigned)N_DIM, (unsigned)B_BYTES, (unsigned)TILE_N);
        printf("       C_full=%ux%u bytes=%u, per tile bytes=%u\n",
               (unsigned)M_DIM, (unsigned)N_DIM, (unsigned)C_BYTES, (unsigned)C_TILE_BYTES);
    }
    flex_global_barrier_xy();

    /* =================== Phase 1: C(0,0) DM initializes A and full B in HBM (preload; not timed) =================== */
    if (IS_DM && P.x == 0u && P.y == 0u) {
        void *raw_a = flex_l1_malloc(A_BYTES + 64u);
        void *raw_b = flex_l1_malloc(B_BYTES + 64u);
        if (!raw_a || !raw_b) {
            printf("[ERR] L1 alloc failed for preload: A=%p B=%p\n", raw_a, raw_b);
            flex_eoc(1); return 1;
        }
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
    flex_global_barrier_xy();

    /* =================== Phase 2: alloc L1 per worker =================== */
    if (is_timer_master) { flex_timer_start(); }
    void *raw_A = 0, *raw_Bt = 0, *raw_Ct = 0;
    void *aln_A = 0, *aln_Bt = 0, *aln_Ct = 0;
    uint32_t off_A = 0, off_Bt = 0, off_Ct = 0;
    float *A = 0, *Btile = 0, *Ctile = 0;

    if (DO_WORK) {
        // printf("[C(0,%u) DM] Phase2: L1 alloc start\n", (unsigned)P.x);
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
    if (is_timer_master) { flex_timer_end(); }
    flex_global_barrier_xy();

    /* =================== Phase 2b: serialized HBM reads (DATA-IN, timed) =================== */
    if (is_timer_master) { flex_timer_start(); }  /* DATA-IN window start */

    /* checksums to print AFTER the timer window */
    uint64_t sa_local = 0, sb_local = 0;

    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            /* Pull A (contiguous 1D) */
            // printf("[HBM][Read ] A  <- 0x%08x (%u B)\n", (unsigned)HBM_A_BASE_OFFSET, (unsigned)A_BYTES);
            dma_read_1d_from_hbm(off_A, (uint32_t)HBM_A_BASE_OFFSET, A_BYTES);

            /* Pull B N-tile via 2D gather:
             *   rows      = B_TILE_ROWS = K_DIM (256)
             *   rowSize   = TILE_N * 4
             *   srcStride = N_DIM  * 4
             *   dstStride = rowSize
             *   srcBase   = HBM_B_BASE_OFFSET + (rx*TILE_N)*4
             */
            const uint32_t src_base = (uint32_t)HBM_B_BASE_OFFSET + (rx * TILE_N) * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(TILE_N * ELEM_BYTES);
            const uint32_t dstStr   = rowSize;
            const uint32_t srcStr   = (uint32_t)(N_DIM * ELEM_BYTES);
            const uint32_t rows     = (uint32_t)B_TILE_ROWS;

            // printf("[HBM][Read ] Btile(N%u..%u) <- 0x%08x (rows=%u, rowSize=%u, srcStride=%u)\n",
            //        (unsigned)N0, (unsigned)(N0 + TILE_N - 1u),
            //        (unsigned)src_base, (unsigned)rows,
            //        (unsigned)rowSize, (unsigned)srcStr);

            bare_dma_start_2d(/*dst*/ local(off_Bt), /*src*/ hbm_addr(src_base),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();

            sa_local = addsum_u32(A,     A_BYTES);
            sb_local = addsum_u32(Btile, B_TILE_BYTES);
            /* (Do not print inside the timer window) */
        }

        flex_global_barrier_xy();
    }

    if (is_timer_master) { flex_timer_end(); }    /* DATA-IN window end */

    /* Ordered checksum prints (outside timer window) */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (DO_WORK && (P.x == rx)) {
            printf("[CHK][C(0,%u)] add(A)=0x%08x%08x  add(Btile)=0x%08x%08x\n",
                   (unsigned)rx,
                   (unsigned)(sa_local >> 32), (unsigned)(sa_local & 0xFFFFFFFFu),
                   (unsigned)(sb_local >> 32), (unsigned)(sb_local & 0xFFFFFFFFu));
        }
    }
    flex_global_barrier_xy();

    /* =================== Sync barrier after reads (SYNC, timed) =================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* =================== Phase 3: compute per-cluster N-tile (COMPUTE, timed) =================== */
    uint64_t sc_local = 0;
    if (is_timer_master) { flex_timer_start(); }
    if (DO_WORK) {
        compute_gemm_tile(A, Btile, Ctile);
        sc_local = addsum_u32(Ctile, C_TILE_BYTES);
        /* (Do not print inside the timer window) */
    }
    if (is_timer_master) { flex_timer_end(); }

    /* Ordered print of compute checksum */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (DO_WORK && (P.x == rx)) {
            printf("[CHK][C(0,%u)] add(Ctile)=0x%08x%08x\n",
                   (unsigned)rx,
                   (unsigned)(sc_local >> 32), (unsigned)(sc_local & 0xFFFFFFFFu));
        }
    }
    flex_global_barrier_xy();

    /* =================== Sync barrier after compute (SYNC, timed) =================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* =================== Phase 4: serialized HBM stores (DATA-OUT, timed) =================== */
    if (is_timer_master) { flex_timer_start(); }  /* DATA-OUT window start */

    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            /* 2D scatter to full C buffer:
             *   rows      = C_TILE_ROWS = M_DIM (64)
             *   rowSize   = TILE_N * 4
             *   dstStride = N_DIM  * 4
             *   srcStride = rowSize
             *   dstBase   = HBM_C_BASE_OFFSET + (rx*TILE_N)*4
             */
            const uint32_t dst_base = (uint32_t)HBM_C_BASE_OFFSET + (rx * TILE_N) * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(TILE_N * ELEM_BYTES);
            const uint32_t dstStr   = (uint32_t)(N_DIM * ELEM_BYTES);
            const uint32_t srcStr   = rowSize;
            const uint32_t rows     = (uint32_t)C_TILE_ROWS;

            // printf("[HBM][Write] Ctile(N%u..%u) -> 0x%08x (rows=%u, rowSize=%u, dstStride=%u)\n",
            //        (unsigned)N0, (unsigned)(N0 + TILE_N - 1u),
            //        (unsigned)dst_base, (unsigned)rows,
            //        (unsigned)rowSize, (unsigned)dstStr);

            bare_dma_start_2d(/*dst*/ hbm_addr(dst_base), /*src*/ local(off_Ct),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();
        }

        flex_global_barrier_xy();
    }

    if (is_timer_master) { flex_timer_end(); }    /* DATA-OUT window end */

    /* =================== Final sync (SYNC, timed) =================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    if (cid == 0u && core == 0u) {
        printf("[Done][Bmk2-2D] All four C tiles stored to HBM base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
