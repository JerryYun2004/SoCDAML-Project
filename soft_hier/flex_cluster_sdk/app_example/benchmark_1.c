/* benchmark_1.c — 3D microbenchmark on Cluster 0 only
 * developed from main_test.c
 * Tensors (FP32):
 *   A[a,b,t] : 16 x 16 x 64    (contiguous)
 *   B[b,t,c] : 16 x 64 x 16    (contiguous)
 *   C[a,b,c] : 16 x 16 x 16    where C[a,b,c] = sum_t A[a,b,t] * B[b,t,c]
 *
 * What this program does:
 *   1) Cluster (0,0), core 0 allocates L1 for A, B, C.
 *   2) Fills A & B with non-zero test patterns in L1, then writes them to HBM.
 *   3) Zeroes A & B in L1, then reloads them from HBM back to L1 (the "load" path).
 *   4) Computes C in L1.
 *   5) Writes C back to HBM.
 *
 * Notes:
 * - Only C(0,0) DM core does work; others exit to avoid DMA misuse.
 * - Uses printf for debug logs (flex_printf.h provides a tiny printf).
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"

/* ---------- 3D sizes (scaled down to fit L1 comfortably) ---------- */
enum {
    A_DIM_A = 16,   /* was 64 */
    A_DIM_B = 16,   /* was 64 */
    T_DIM   = 64,   /* was 256 */
    C_DIM_C = 16    /* was 64 */
};

/* derived sizes (elements) */
#define A_ELEMS   ((uint32_t)(A_DIM_A * A_DIM_B * T_DIM))
#define B_ELEMS   ((uint32_t)(A_DIM_B * T_DIM   * C_DIM_C))
#define C_ELEMS   ((uint32_t)(A_DIM_A * A_DIM_B * C_DIM_C))

/* FP32 */
#define ELEM_BYTES 4u

/* derived sizes (bytes) */
#define A_BYTES    (A_ELEMS * ELEM_BYTES)
#define B_BYTES    (B_ELEMS * ELEM_BYTES)
#define C_BYTES    (C_ELEMS * ELEM_BYTES)

/* ---------- helpers ---------- */
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

/* ---------- simple 3D compute: C[a,b,c] = sum_t A[a,b,t] * B[b,t,c] ---------- */
static void compute_3d(const float *A, const float *B, float *C)
{
    for (uint32_t a = 0; a < (uint32_t)A_DIM_A; ++a) {
        for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b) {
            for (uint32_t c = 0; c < (uint32_t)C_DIM_C; ++c) {
                float acc = 0.0f;
                for (uint32_t t = 0; t < (uint32_t)T_DIM; ++t) {
                    /* idx(A[a,b,t]) = ((a*A_DIM_B + b)*T_DIM + t) */
                    const uint32_t ia = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)T_DIM + t;
                    /* idx(B[b,t,c]) = ((b*T_DIM + t)*C_DIM_C + c) */
                    const uint32_t ib = ((b * (uint32_t)T_DIM) + t) * (uint32_t)C_DIM_C + c;
                    acc += A[ia] * B[ib];
                }
                /* idx(C[a,b,c]) = ((a*A_DIM_B + b)*C_DIM_C + c) */
                const uint32_t ic = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)C_DIM_C + c;
                C[ic] = acc;
            }
        }
    }
}

/* ---------- fill test patterns (non-zero) ---------- */
static void fill_A_pattern(float *A)
{
    /* A[a,b,t] = 0.001f*a + 0.01f*b + 1.0f + 0.0001f*t */
    for (uint32_t a = 0; a < (uint32_t)A_DIM_A; ++a)
    for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b)
    for (uint32_t t = 0; t < (uint32_t)T_DIM;   ++t) {
        const uint32_t ia = ((a * (uint32_t)A_DIM_B) + b) * (uint32_t)T_DIM + t;
        A[ia] = 1.0f + 0.001f*(float)a + 0.01f*(float)b + 0.0001f*(float)t;
    }
}

static void fill_B_pattern(float *B)
{
    /* B[b,t,c] = 0.5f + 0.02f*b + 0.0002f*t + 0.003f*c */
    for (uint32_t b = 0; b < (uint32_t)A_DIM_B; ++b)
    for (uint32_t t = 0; t < (uint32_t)T_DIM;   ++t)
    for (uint32_t c = 0; c < (uint32_t)C_DIM_C; ++c) {
        const uint32_t ib = ((b * (uint32_t)T_DIM) + t) * (uint32_t)C_DIM_C + c;
        B[ib] = 0.5f + 0.02f*(float)b + 0.0002f*(float)t + 0.003f*(float)c;
    }
}

/* ===================================================================================== */

int main(void)
{
    /* Bring-up */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x,P.y in [0..3] */

    /* Tell the user what we’re doing */
    printf("[Info][3D] Using 3D tensors (FP32)\n");
    printf("  A: %ux%ux%u  (%u bytes)\n", (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)T_DIM,   (unsigned)A_BYTES);
    printf("  B: %ux%u x%u (%u bytes)\n", (unsigned)A_DIM_B, (unsigned)T_DIM,   (unsigned)C_DIM_C, (unsigned)B_BYTES);
    printf("  C: %ux%ux%u  (%u bytes)\n", (unsigned)A_DIM_A, (unsigned)A_DIM_B, (unsigned)C_DIM_C, (unsigned)C_BYTES);

    /* ---------- L1 allocations (64B-aligned) ---------- */
    void *raw_a = flex_l1_malloc(A_BYTES + 64u);
    void *raw_b = flex_l1_malloc(B_BYTES + 64u);
    void *raw_c = flex_l1_malloc(C_BYTES + 64u);
    if (!raw_a || !raw_b || !raw_c) {
        printf("[ERR] L1 malloc failed: A=%p B=%p C=%p\n", raw_a, raw_b, raw_c);
        flex_eoc(1);
        return 1;
    }
    void *aln_a = align64(raw_a);
    void *aln_b = align64(raw_b);
    void *aln_c = align64(raw_c);

    uint32_t off_a = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_a);
    uint32_t off_b = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_b);
    uint32_t off_c = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, aln_c);

    printf("[L1] Offsets: A=%u B=%u C=%u\n", (unsigned)off_a, (unsigned)off_b, (unsigned)off_c);

    float *A = (float*)(uintptr_t)local(off_a);
    float *B = (float*)(uintptr_t)local(off_b);
    float *C = (float*)(uintptr_t)local(off_c);

    /* ---------- Step 1+2: Fill A,B (non-zero) in L1 and write to HBM ---------- */
    fill_A_pattern(A);
    fill_B_pattern(B);

    /* Choose HBM base offsets (use fixed_proj.h regions) */
    const uint32_t H_OFF_A = (uint32_t)HBM_A_BASE_OFFSET;
    const uint32_t H_OFF_B = (uint32_t)HBM_B_BASE_OFFSET;
    const uint32_t H_OFF_C = (uint32_t)HBM_C_BASE_OFFSET;

    printf("[HBM][Write] A -> off=0x%08x bytes=%u\n", (unsigned)H_OFF_A, (unsigned)A_BYTES);
    dma_write_1d_to_hbm(H_OFF_A, off_a, A_BYTES);

    printf("[HBM][Write] B -> off=0x%08x bytes=%u\n", (unsigned)H_OFF_B, (unsigned)B_BYTES);
    dma_write_1d_to_hbm(H_OFF_B, off_b, B_BYTES);

    /* ---------- Step 3: Zero L1 A,B then load from HBM back to L1 ---------- */
    zero32(A, A_BYTES);
    zero32(B, B_BYTES);

    printf("[HBM][Read ] A <- off=0x%08x bytes=%u\n", (unsigned)H_OFF_A, (unsigned)A_BYTES);
    dma_read_1d_from_hbm(off_a, H_OFF_A, A_BYTES);

    printf("[HBM][Read ] B <- off=0x%08x bytes=%u\n", (unsigned)H_OFF_B, (unsigned)B_BYTES);
    dma_read_1d_from_hbm(off_b, H_OFF_B, B_BYTES);

    /* Optional integrity checks (small additive checksum) */
    uint64_t sa = addsum_u32(A, A_BYTES);
    uint64_t sb = addsum_u32(B, B_BYTES);
    printf("[CHK] addsum(A)=0x%08x%08x  addsum(B)=0x%08x%08x\n",
           (unsigned)(sa >> 32), (unsigned)(sa & 0xFFFFFFFFu),
           (unsigned)(sb >> 32), (unsigned)(sb & 0xFFFFFFFFu));

    /* ---------- Step 4: Compute C in L1 ---------- */
    zero32(C, C_BYTES);
    compute_3d(A, B, C);

    uint64_t sc = addsum_u32(C, C_BYTES);
    printf("[CHK] addsum(C)=0x%08x%08x\n", (unsigned)(sc >> 32), (unsigned)(sc & 0xFFFFFFFFu));

    /* ---------- Step 5: Store C back to HBM ---------- */
    printf("[HBM][Write] C -> off=0x%08x bytes=%u\n", (unsigned)H_OFF_C, (unsigned)C_BYTES);
    dma_write_1d_to_hbm(H_OFF_C, off_c, C_BYTES);

    printf("[Done] 3D benchmark complete.\n");

    flex_eoc(0);
    return 0;
}
