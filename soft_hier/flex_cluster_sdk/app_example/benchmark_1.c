/* main_test.c — 3D microbenchmark; no early exit, only C(0,0) DM does work
 *
 * Tensors (FP32):
 *   A[a,b,t] : 16 x 16 x 64
 *   B[b,t,c] : 16 x 64 x 16
 *   C[a,b,c] : 16 x 16 x 16,  C[a,b,c] = sum_t A[a,b,t] * B[b,t,c]
 *
 * Phases:
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
#include "flex_printf.h"  /* provides printf */

enum {
    A_DIM_A = 16,
    A_DIM_B = 16,
    T_DIM   = 64,
    C_DIM_C = 16
};

/* FP32 */
#define ELEM_BYTES 4u

/* elements / bytes */
#define A_ELEMS   ((uint32_t)(A_DIM_A * A_DIM_B * T_DIM))
#define B_ELEMS   ((uint32_t)(A_DIM_B * T_DIM   * C_DIM_C))
#define C_ELEMS   ((uint32_t)(A_DIM_A * A_DIM_B * C_DIM_C))
#define A_BYTES   (A_ELEMS * ELEM_BYTES)
#define B_BYTES   (B_ELEMS * ELEM_BYTES)
#define C_BYTES   (C_ELEMS * ELEM_BYTES)

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
static void fill_A_pattern(float *A)
{
    /* A[a,b,t] = 1.0 + 0.001*a + 0.01*b + 0.0001*t */
    for (uint32_t a = 0; a < (uint32_t)A_DIM_A; ++a)
    for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b)
    for (uint32_t t = 0; t < (uint32_t)T_DIM;   ++t) {
        const uint32_t ia = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)T_DIM + t;
        A[ia] = 1.0f + 0.001f*(float)a + 0.01f*(float)b + 0.0001f*(float)t;
    }
}

static void fill_B_pattern(float *B)
{
    /* B[b,t,c] = 0.5 + 0.02*b + 0.0002*t + 0.003*c */
    for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b)
    for (uint32_t t = 0; t < (uint32_t)T_DIM;   ++t)
    for (uint32_t c = 0; c < (uint32_t)C_DIM_C; ++c) {
        const uint32_t ib = ((b * (uint32_t)T_DIM) + t) * (uint32_t)C_DIM_C + c;
        B[ib] = 0.5f + 0.02f*(float)b + 0.0002f*(float)t + 0.003f*(float)c;
    }
}

/* C[a,b,c] = sum_t A[a,b,t] * B[b,t,c] */
static void compute_3d(const float *A, const float *B, float *C)
{
    for (uint32_t a = 0; a < (uint32_t)A_DIM_A; ++a) {
        for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b) {
            for (uint32_t c = 0; c < (uint32_t)C_DIM_C; ++c) {
                float acc = 0.0f;
                for (uint32_t t = 0; t < (uint32_t)T_DIM; ++t) {
                    const uint32_t ia = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)T_DIM + t;
                    const uint32_t ib = ((b * (uint32_t)T_DIM) + t) * (uint32_t)C_DIM_C + c;
                    acc += A[ia] * B[ib];
                }
                const uint32_t ic = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)C_DIM_C + c;
                C[ic] = acc;
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

    /* Only C(0,0) DM does the work; everyone else stays in step via barriers. */
    const uint32_t DO_WORK = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u));

    if (cid == 0u && core == 0u) {
        printf("[Info][3D] Single-cluster (0,0) 3D test; others idle but synchronized\n");
        printf("       A=%ux%ux%u  (%u bytes)\n", (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)T_DIM,   (unsigned)A_BYTES);
        printf("       B=%ux%ux%u  (%u bytes)\n", (unsigned)A_DIM_B, (unsigned)T_DIM,   (unsigned)C_DIM_C, (unsigned)B_BYTES);
        printf("       C=%ux%ux%u  (%u bytes)\n", (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)C_DIM_C, (unsigned)C_BYTES);
    }

    flex_global_barrier_xy();

    /* L1 allocations (only C(0,0) DM) */
    void *raw_a = 0, *raw_b = 0, *raw_c = 0;
    void *aln_a = 0, *aln_b = 0, *aln_c = 0;
    uint32_t off_a = 0, off_b = 0, off_c = 0;
    float *A = 0, *B = 0, *C = 0;

    if (DO_WORK) {
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
        printf("[L1] Offsets (C00 DM): A=%u B=%u C=%u\n", (unsigned)off_a, (unsigned)off_b, (unsigned)off_c);
    }

    if (DO_WORK) {flex_timer_start();}
    flex_global_barrier_xy();
    if (DO_WORK) {flex_timer_end();}

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
    
    if (DO_WORK) {flex_timer_start();}
    flex_global_barrier_xy();
    if (DO_WORK) {flex_timer_end();}

    /* Clear L1 A,B and Read back from HBM */
    if (DO_WORK) {
        zero32(A, A_BYTES);
        zero32(B, B_BYTES);

        flex_timer_start();
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
    
    if (DO_WORK) {flex_timer_start();}
    flex_global_barrier_xy();
    if (DO_WORK) {flex_timer_end();}

    /* Compute C in L1 (C00 DM) */
    if (DO_WORK) {
        flex_timer_start();
        zero32(C, C_BYTES);
        compute_3d(A, B, C);
        uint64_t sc = addsum_u32(C, C_BYTES);
        flex_timer_end();
        printf("[CHK] addsum(C)=0x%08x%08x\n", (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));
    }
    
    if (DO_WORK) {flex_timer_start();}
    flex_global_barrier_xy();
    if (DO_WORK) {flex_timer_end();}

    /* Store C to HBM (C00 DM) */
    if (DO_WORK) {
        const uint32_t H_OFF_C = (uint32_t)HBM_C_BASE_OFFSET;
        printf("[HBM][Write] C -> 0x%08x (%u bytes)\n", (unsigned)H_OFF_C, (unsigned)C_BYTES);
        dma_write_1d_to_hbm(H_OFF_C, off_c, C_BYTES);
        printf("[Done] 3D benchmark complete (C(0,0) DM).\n");
    }

    if (DO_WORK) {flex_timer_start();}
    flex_global_barrier_xy();
    if (DO_WORK) {flex_timer_end();}

    /* Everyone exits together */
    flex_eoc(0);
    return 0;
}
