/* benchmark_1.c — 2D GEMM microbenchmark; no early exit, only C(0,0) DM does work
 *
 * Tensors (FP32):
 *   A[M,K] : 64  x 256   (row-major)
 *   B[K,N] : 256 x 64    (row-major)
 *   C[M,N] : 64  x 64,   C = A * B
 *
 * Phases (kept identical to your previous flow):
 *   - Bring-up + global barriers
 *   - L1 alloc (C(0,0) DM only) + report offsets
 *   - Initialize A,B in L1 -> write to HBM (C(0,0) DM only)
 *   - Clear L1 A,B then read back from HBM (C(0,0) DM only)
 *   - Compute C in L1 (C(0,0) DM only)
 *   - Write C to HBM (C(0,0) DM only)
 *   - Everyone reaches final barrier and exits together (no early exit)
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"   /* provides printf */

/* 2D GEMM dims */
enum {
    M_DIM = 64,     /* rows of A and C */
    K_DIM = 256,    /* cols of A / rows of B */
    N_DIM = 64      /* cols of B and C */
};

/* FP32 */
#define ELEM_BYTES 4u

/* elements / bytes */
#define A_ELEMS  ((uint32_t)(M_DIM * K_DIM))   /* 64*256  */
#define B_ELEMS  ((uint32_t)(K_DIM * N_DIM))   /* 256*64  */
#define C_ELEMS  ((uint32_t)(M_DIM * N_DIM))   /* 64*64   */
#define A_BYTES  (A_ELEMS * ELEM_BYTES)        /* 65536   */
#define B_BYTES  (B_ELEMS * ELEM_BYTES)        /* 65536   */
#define C_BYTES  (C_ELEMS * ELEM_BYTES)        /* 16384   */

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

/* Fill test patterns (non-zero) */
static void fill_A_pattern(float *A)  /* A[M,K] row-major */
{
    /* A[i,k] = 1.0 + 0.001*i + 0.0001*k */
    for (uint32_t i = 0; i < (uint32_t)M_DIM; ++i)
    for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k) {
        const uint32_t ia = i * (uint32_t)K_DIM + k;
        A[ia] = 1.0f + 0.001f*(float)i + 0.0001f*(float)k;
    }
}

static void fill_B_pattern(float *B)  /* B[K,N] row-major */
{
    /* B[k,j] = 0.5 + 0.002*k + 0.01*j */
    for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k)
    for (uint32_t j = 0; j < (uint32_t)N_DIM; ++j) {
        const uint32_t ib = k * (uint32_t)N_DIM + j;
        B[ib] = 0.5f + 0.002f*(float)k + 0.01f*(float)j;
    }
}

/* C = A * B  where A[M,K], B[K,N], C[M,N] (all row-major) */
static void compute_2d_gemm(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < (uint32_t)M_DIM; ++i) {
        float *c_row = &C[i * (uint32_t)N_DIM];
        const float *a_row = &A[i * (uint32_t)K_DIM];
        for (uint32_t j = 0; j < (uint32_t)N_DIM; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < (uint32_t)K_DIM; ++k) {
                /* a_row[k] * B[k, j] */
                acc += a_row[k] * B[k * (uint32_t)N_DIM + j];
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

    /* Only C(0,0) DM does the work; everyone else stays in step via barriers. */
    const uint32_t DO_WORK = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u));

    if (cid == 0u && core == 0u) {
        printf("[Info][2D] Single-cluster (0,0) GEMM test; others idle but synchronized\n");
        printf("       A=%ux%u  (%u bytes)\n", (unsigned)M_DIM, (unsigned)K_DIM, (unsigned)A_BYTES);
        printf("       B=%ux%u  (%u bytes)\n", (unsigned)K_DIM, (unsigned)N_DIM, (unsigned)B_BYTES);
        printf("       C=%ux%u  (%u bytes)\n", (unsigned)M_DIM, (unsigned)N_DIM, (unsigned)C_BYTES);
    }
    flex_global_barrier_xy();

    /* L1 allocations (only C(0,0) DM) */
    void *raw_a = 0, *raw_b = 0, *raw_c = 0;
    void *aln_a = 0, *aln_b = 0, *aln_c = 0;
    uint32_t off_a = 0, off_b = 0, off_c = 0;
    float *A = 0, *B = 0, *C = 0;

    if (DO_WORK) {
        flex_timer_start();  /* include L1 alloc under data movement */
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

    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    /* Initialize A,B in L1 and write to HBM (C00 DM) */
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

    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    /* Clear L1 A,B and Read back from HBM */
    if (DO_WORK) {
        zero32(A, A_BYTES);
        zero32(B, B_BYTES);

        flex_timer_start();  /* data movement timing window */
        const uint32_t H_OFF_A = (uint32_t)HBM_A_BASE_OFFSET;
        const uint32_t H_OFF_B = (uint32_t)HBM_B_BASE_OFFSET;

        printf("[HBM][Read ] A <- 0x%08x (%u bytes)\n", (unsigned)H_OFF_A, (unsigned)A_BYTES);
        dma_read_1d_from_hbm(off_a, H_OFF_A, A_BYTES);

        printf("[HBM][Read ] B <- 0x%08x (%u bytes)\n", (unsigned)H_OFF_B, (unsigned)B_BYTES);
        dma_read_1d_from_hbm(off_b, H_OFF_B, B_BYTES);

        uint64_t sa = addsum_u32(A, A_BYTES);
        uint64_t sb = addsum_u32(B, B_BYTES);
        flex_timer_end();
        printf("[CHK] addsum(A)=0x%08x%08x  addsum(B)=0x%08x%08x\n",
               (unsigned)(sa >> 32), (unsigned)(sa & 0xFFFFFFFFu),
               (unsigned)(sb >> 32), (unsigned)(sb & 0xFFFFFFFFu));
    }

    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    /* Compute C in L1 (C00 DM) */
    if (DO_WORK) {
        flex_timer_start();  /* compute timing window */
        zero32(C, C_BYTES);
        compute_2d_gemm(A, B, C);
        uint64_t sc = addsum_u32(C, C_BYTES);
        flex_timer_end();
        printf("[CHK] addsum(C)=0x%08x%08x\n",
               (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));
    }

    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    /* Store C to HBM (C00 DM) */
    if (DO_WORK) {
        const uint32_t H_OFF_C = (uint32_t)HBM_C_BASE_OFFSET;
        printf("[HBM][Write] C -> 0x%08x (%u bytes)\n", (unsigned)H_OFF_C, (unsigned)C_BYTES);
        dma_write_1d_to_hbm(H_OFF_C, off_c, C_BYTES);
        printf("[Done] 2D benchmark complete (C(0,0) DM).\n");
    }

    if (DO_WORK) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (DO_WORK) { flex_timer_end(); }

    /* Everyone exits together */
    flex_eoc(0);
    return 0;
}
