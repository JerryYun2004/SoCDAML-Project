/*
 * row_direct.c -- cleaned benchmark_2.c from Jerry Yun's SoCDAML project.
 * Source revision: 707cc6f0db370d16f608de7b9ff86e9c758d95e9
 *
 * PURPOSE: Row direct (slide 10): four workers independently fetch shared A and their B tiles.
 * CURRENT SOURCE: A[64,256] * B[256,256] -> C[64,256], FP32, K=256.
 * Each active cluster computes a 64x64 output on its DM core using scalar C.
 * The slide workload has K=64 and describes Spatz; that historical kernel and
 * configuration have not been recovered. This file is the audited source
 * baseline, not a verified reproduction of the slide timings.
 *
 * Coordinates are C[y,x]. Matrices are row-major. The hardware is a 4x4 grid;
 * idle cores still join global barriers. Worker eligibility is explicit below.
 * HBM input initialization is outside the measured DMA/compute intervals.
 * Existing timer/barrier placement is preserved; interval names are comments
 * only. A master-local compute interval is not the slowest worker's runtime.
 * See gemm/README.md and docs/project/REPRODUCTION.md for boundaries and limits.
 *
 * Attribution: original benchmark by Jerry Yun, using the ETH Zurich / PULP
 * SoftHier SDK and GVSoC. Existing dependency authorship/licenses still apply.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"

/* M: output rows; K: reduction length; N: output columns. */
enum {
    M_DIM   = 64u,
    K_DIM   = 256u,
    N_DIM   = 256u,
    TILE_N  = 64u
};

#ifndef ELEM_BYTES
#define ELEM_BYTES 4u
#endif

#define A_ELEMS   (M_DIM * K_DIM)
#define B_ELEMS   (K_DIM * N_DIM)
#define C_ELEMS   (M_DIM * N_DIM)
#define A_BYTES   (A_ELEMS * ELEM_BYTES)
#define B_BYTES   (B_ELEMS * ELEM_BYTES)
#define C_BYTES   (C_ELEMS * ELEM_BYTES)

#define B_TILE_ROWS   (K_DIM)
#define B_TILE_BYTES  (B_TILE_ROWS * TILE_N * ELEM_BYTES)
#define ROW_C_TILE_ROWS   (M_DIM)
#define C_TILE_BYTES  (ROW_C_TILE_ROWS * TILE_N * ELEM_BYTES)

/* DMA buffers use 64-byte alignment. Retain the raw allocation pointer for free(). */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void*)v;
}

/* Clear bytes through volatile words; used to expose missing transfers in the single-cluster case. */
static inline void zero32(void *dst, uint32_t nbytes)
{
    volatile uint32_t *p = (volatile uint32_t*)dst;
    for (uint32_t i = 0; i < (nbytes >> 2); ++i) p[i] = 0u;
}

/* Legacy additive checksum of FP32 bit patterns. This is not a numerical correctness test. */
static inline uint64_t addsum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint32_t n = n_bytes >> 2;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += (uint64_t)p[i];
    return s;
}

/* Blocking DMA from this cluster's L1 offset to an HBM offset. */
static inline void dma_write_1d_to_hbm(uint32_t hbm_off, uint32_t l1_off, uint32_t bytes)
{
    bare_dma_start_1d(hbm_addr(hbm_off), local(l1_off), bytes);
    bare_dma_wait_all();
}

/* Blocking DMA from HBM to this cluster's L1 offset. */
static inline void dma_read_1d_from_hbm(uint32_t l1_off, uint32_t hbm_off, uint32_t bytes)
{
    bare_dma_start_1d(local(l1_off), hbm_addr(hbm_off), bytes);
    bare_dma_wait_all();
}

/* Deterministic FP32 input: A[i,k] = 1 + 0.001*i + 0.0001*k. */
static void fill_A_pattern(float *A)
{
    for (uint32_t i = 0; i < M_DIM; ++i)
    for (uint32_t k = 0; k < K_DIM; ++k) {
        const uint32_t ia = i * K_DIM + k;
        A[ia] = 1.0f + 0.001f*(float)i + 0.0001f*(float)k;
    }
}

/* Initialize the full HBM B matrix before dividing it into worker column tiles. */
static void fill_B_full_pattern(float *B)
{
    for (uint32_t k = 0; k < K_DIM; ++k)
    for (uint32_t j = 0; j < N_DIM; ++j) {
        const uint32_t ib = k * N_DIM + j;
        B[ib] = 0.5f + 0.002f*(float)k + 0.01f*(float)j;
    }
}

/* Original scalar FP32 GEMM loop. Only a worker DM core calls it; no Spatz dispatch occurs. */
static void gemm_scalar_fp32(const float *A, const float *Btile, float *Ctile)
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

/* Every core must follow the same global-barrier sequence, even when idle. */
int main(void)
{
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t IS_DM = flex_is_dm_core();

    const uint32_t DO_WORK         = (uint32_t)(IS_DM && (P.y == 0u) && (P.x < 4u));
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
        /* Both preload writes have completed. Return raw buffers in reverse order
         * so all workers begin from the same free-heap layout. */
        flex_l1_free(raw_b);
        flex_l1_free(raw_a);
    }
    flex_global_barrier_xy();

    /* Timer 1: allocation. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    void *raw_A = 0, *raw_Bt = 0, *raw_Ct = 0;
    void *aln_A = 0, *aln_Bt = 0, *aln_Ct = 0;
    uint32_t off_A = 0, off_Bt = 0, off_Ct = 0;
    float *A = 0, *Btile = 0, *Ctile = 0;

    if (DO_WORK) {
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

    /* Timer 2: input_dma_with_barriers. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }

    /* All cores participate; only worker x=rx performs this slot's work. */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            dma_read_1d_from_hbm(off_A, (uint32_t)HBM_A_BASE_OFFSET, A_BYTES);

            const uint32_t src_base = (uint32_t)HBM_B_BASE_OFFSET + (rx * TILE_N) * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(TILE_N * ELEM_BYTES);
            const uint32_t dstStr   = rowSize;
            const uint32_t srcStr   = (uint32_t)(N_DIM * ELEM_BYTES);
            const uint32_t rows     = (uint32_t)B_TILE_ROWS;

            /* 2D DMA arguments: destination, source, bytes per row,
             * destination row stride, source row stride, row count.
             * Gather packs a B column tile; scatter writes a C column tile. */
            bare_dma_start_2d(local(off_Bt), hbm_addr(src_base),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();
        }

        flex_global_barrier_xy();
    }

    if (is_timer_master) { flex_timer_end(); }

    /* Timer 3: sync_after_input. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    uint64_t sc_local = 0;
    /* Timer 4: compute_and_checksum. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    if (DO_WORK) {
        gemm_scalar_fp32(A, Btile, Ctile);
        sc_local = addsum_u32(Ctile, C_TILE_BYTES);
    }
    if (is_timer_master) { flex_timer_end(); }

    /* All cores participate; only worker x=rx performs this slot's work. */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (DO_WORK && (P.x == rx)) {
            printf("[CHK][C(0,%u)] add(Ctile)=0x%08x%08x\n",
                   (unsigned)rx,
                   (unsigned)(sc_local >> 32), (unsigned)(sc_local & 0xFFFFFFFFu));
        }
    }
    flex_global_barrier_xy();

    /* Timer 5: sync_after_checksum_prints. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 6: output_dma_with_barriers. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }

    /* All cores participate; only worker x=rx performs this slot's work. */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();

        if (DO_WORK && (P.x == rx)) {
            const uint32_t dst_base = (uint32_t)HBM_C_BASE_OFFSET + (rx * TILE_N) * ELEM_BYTES;
            const uint32_t rowSize  = (uint32_t)(TILE_N * ELEM_BYTES);
            const uint32_t dstStr   = (uint32_t)(N_DIM * ELEM_BYTES);
            const uint32_t srcStr   = rowSize;
            const uint32_t rows     = (uint32_t)ROW_C_TILE_ROWS;

            /* 2D DMA arguments: destination, source, bytes per row,
             * destination row stride, source row stride, row count.
             * Gather packs a B column tile; scatter writes a C column tile. */
            bare_dma_start_2d(hbm_addr(dst_base), local(off_Ct),
                              rowSize, dstStr, srcStr, rows);
            bare_dma_wait_all();
        }

        flex_global_barrier_xy();
    }

    if (is_timer_master) { flex_timer_end(); }

    /* Timer 7: final_sync. Master C[0,0] DM only. */
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
