/*
 * single_cluster.c -- cleaned benchmark_1.c from Jerry Yun's SoCDAML project.
 * Source revision: 707cc6f0db370d16f608de7b9ff86e9c758d95e9
 *
 * PURPOSE: Single-cluster baseline (slide 8): one C[0,0] worker computes a 64x64 output.
 * CURRENT SOURCE: A[64,256] * B[256,64] -> C[64,64], FP32, K=256.
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
    M_DIM = 64,
    K_DIM = 256,
    N_DIM = 64
};

#if ELEM_BYTES != 4u
#error "This FP32 benchmark requires four-byte elements."
#endif

#define A_ELEMS  ((uint32_t)(M_DIM * K_DIM))
#define B_ELEMS  ((uint32_t)(K_DIM * N_DIM))
#define C_ELEMS  ((uint32_t)(M_DIM * N_DIM))
#define A_BYTES  (A_ELEMS * ELEM_BYTES)
#define B_BYTES  (B_ELEMS * ELEM_BYTES)
#define C_BYTES  (C_ELEMS * ELEM_BYTES)

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
    for (uint32_t i = 0; i < (uint32_t)M_DIM; ++i)
    for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k) {
        const uint32_t ia = i * (uint32_t)K_DIM + k;
        A[ia] = 1.0f + 0.001f*(float)i + 0.0001f*(float)k;
    }
}

/* Deterministic FP32 input: B[k,j] = 0.5 + 0.002*k + 0.01*j. */
static void fill_B_pattern(float *B)
{
    for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k)
    for (uint32_t j = 0; j < (uint32_t)N_DIM; ++j) {
        const uint32_t ib = k * (uint32_t)N_DIM + j;
        B[ib] = 0.5f + 0.002f*(float)k + 0.01f*(float)j;
    }
}

/* Original scalar FP32 GEMM loop. Only a worker DM core calls it; no Spatz dispatch occurs. */
static void gemm_scalar_fp32(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < (uint32_t)M_DIM; ++i) {
        float *c_row = &C[i * (uint32_t)N_DIM];
        const float *a_row = &A[i * (uint32_t)K_DIM];
        for (uint32_t j = 0; j < (uint32_t)N_DIM; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k) {
                acc += a_row[k] * B[k * (uint32_t)N_DIM + j];
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

    const uint32_t DO_WORK = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u));

    if (cid == 0u && core == 0u) {
        printf("[Info][2D] Single-cluster (0,0) GEMM test; others idle but synchronized\n");
        printf("       A=%ux%u  (%u bytes)\n", (unsigned)M_DIM, (unsigned)K_DIM, (unsigned)A_BYTES);
        printf("       B=%ux%u  (%u bytes)\n", (unsigned)K_DIM, (unsigned)N_DIM, (unsigned)B_BYTES);
        printf("       C=%ux%u  (%u bytes)\n", (unsigned)M_DIM, (unsigned)N_DIM, (unsigned)C_BYTES);
    }
    flex_global_barrier_xy();

    void *raw_a = 0, *raw_b = 0, *raw_c = 0;
    void *aln_a = 0, *aln_b = 0, *aln_c = 0;
    uint32_t off_a = 0, off_b = 0, off_c = 0;
    float *A = 0, *B = 0, *C = 0;

    if (DO_WORK) {
        /* Timer 1: allocation. Master C[0,0] DM only. */
        flex_timer_start();
        raw_a = flex_l1_malloc(A_BYTES + 64u);
        raw_b = flex_l1_malloc(B_BYTES + 64u);
        raw_c = flex_l1_malloc(C_BYTES + 64u);
        if (!raw_a || !raw_b || !raw_c) {
            printf("[ERR] L1 malloc failed: A=%p B=%p C=%p\n", raw_a, raw_b, raw_c);
            flex_eoc(1);
            return 1;
        }
        aln_a = align64(raw_a);
        aln_b = align64(raw_b);
        aln_c = align64(raw_c);

        off_a = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_a);
        off_b = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_b);
        off_c = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_c);

        A = (float*)(uintptr_t)local(off_a);
        B = (float*)(uintptr_t)local(off_b);
        C = (float*)(uintptr_t)local(off_c);
        flex_timer_end();

        printf("[L1] Offsets (C00 DM): A=%u B=%u C=%u\n",
               (unsigned)off_a, (unsigned)off_b, (unsigned)off_c);
    }

    /* Timer 2: sync_after_allocation. Master C[0,0] DM only. */
    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    if (DO_WORK) {
        fill_A_pattern(A);
        fill_B_pattern(B);

        const uint32_t H_OFF_A = (uint32_t)HBM_A_BASE_OFFSET;
        const uint32_t H_OFF_B = (uint32_t)HBM_B_BASE_OFFSET;

        printf("[HBM][Write] A -> 0x%08x (%u bytes)\n", (unsigned)H_OFF_A, (unsigned)A_BYTES);
        dma_write_1d_to_hbm(H_OFF_A, off_a, A_BYTES);

        printf("[HBM][Write] B -> 0x%08x (%u bytes)\n", (unsigned)H_OFF_B, (unsigned)B_BYTES);
        dma_write_1d_to_hbm(H_OFF_B, off_b, B_BYTES);
    }

    /* Timer 3: sync_after_preload. Master C[0,0] DM only. */
    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    if (DO_WORK) {
        zero32(A, A_BYTES);
        zero32(B, B_BYTES);

        /* Timer 4: input_dma. Master C[0,0] DM only. */
        flex_timer_start();
        const uint32_t H_OFF_A = (uint32_t)HBM_A_BASE_OFFSET;
        const uint32_t H_OFF_B = (uint32_t)HBM_B_BASE_OFFSET;

        dma_read_1d_from_hbm(off_a, H_OFF_A, A_BYTES);

        dma_read_1d_from_hbm(off_b, H_OFF_B, B_BYTES);

        flex_timer_end();
    }

    /* Timer 5: sync_after_input. Master C[0,0] DM only. */
    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    if (DO_WORK) {
        /* Timer 6: compute_zero_and_checksum. Master C[0,0] DM only. */
        flex_timer_start();
        zero32(C, C_BYTES);
        gemm_scalar_fp32(A, B, C);
        uint64_t sc = addsum_u32(C, C_BYTES);
        flex_timer_end();
        printf("[CHK] addsum(C)=0x%08x%08x\n",
               (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));
    }

    /* Timer 7: sync_after_compute. Master C[0,0] DM only. */
    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    if (DO_WORK) {
        /* Timer 8: output_dma. Master C[0,0] DM only. */
        flex_timer_start();
        const uint32_t H_OFF_C = (uint32_t)HBM_C_BASE_OFFSET;

        dma_write_1d_to_hbm(H_OFF_C, off_c, C_BYTES);

        flex_timer_end();
    }

    /* Timer 9: final_sync. Master C[0,0] DM only. */
    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    flex_eoc(0);
    return 0;
}
