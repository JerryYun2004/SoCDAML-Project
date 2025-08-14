/* main_direct.c — HBM→L1 load timing (no inter-cluster broadcast)
 * Goal: measure the time to load A/B when each cluster fetches its own strips.
 * No prints. No use of the extended runtime header.
 */

#include <stdint.h>
#include "flex_runtime.h"      /* flex_barrier_xy_init, flex_alloc_init, flex_get_* */
#include "flex_dma_pattern.h"  /* bare_dma_start_1d/2d, bare_dma_wait_all, local(), hbm_addr() */
#include "flex_alloc.h"        /* flex_l1_malloc */
#include "soc_daml.h"          /* get_pos(), FlexPosition, tcdm_offset_from_ptr, ARCH_* */

/* ---------------- Problem sizes (match the project) ---------------- */
#define MAT_N        (256u)   /* A,B,C are 256x256 */
#define TILE         (64u)    /* 64-row / 64-col tiles */
#define ELEM_BYTES   (4u)     /* FP32 */
#define BYTES_A_STRIP (TILE * MAT_N * ELEM_BYTES)  /* 64x256x4 = 65536 */
#define BYTES_B_STRIP (TILE * MAT_N * ELEM_BYTES)  /* 256x64x4 = 65536 */

/* ---------------- HBM layout (same as earlier good runs) ----------- */
#define HBM_A_BASE_OFFSET  (0x00000400u)  /* A starts here in HBM */
#define HBM_B_BASE_OFFSET  (0x00040400u)  /* B starts here in HBM */

/* 64B-align helper (kept local so we don't depend on other headers) */
static inline uint32_t align_up_u32(uint32_t v, uint32_t a) { return (v + (a - 1u)) & ~(a - 1u); }

/* Zero a region via a pointer (avoid mixing CPU writes with local(offset)) */
static inline void zero_u32(void *ptr, uint32_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)ptr;
    const uint32_t n = bytes >> 2;
    for (uint32_t i = 0; i < n; ++i) { p[i] = 0u; }
}

/* Offsets of A’s 64x256 row-strips in HBM (by cluster row r=0..3) */
static inline uint32_t hbm_off_A_strip(uint32_t r)
{
    return (uint32_t)(HBM_A_BASE_OFFSET + r * BYTES_A_STRIP);
}

/* Base address in HBM for B’s 256x64 column-strip (by cluster col c=0..3) */
static inline uint32_t hbm_off_B_strip_base(uint32_t c)
{
    /* row-major: col-start = c*64 → byte offset = (c*64) * 4 = 0x100 * c */
    return (uint32_t)(HBM_B_BASE_OFFSET + (c * TILE * ELEM_BYTES));
}

int main(void)
{
    /* Global init */
    flex_barrier_xy_init();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x: 0..3 (col), P.y: 0..3 (row) */
    const uint32_t cx = P.x;
    const uint32_t cy = P.y;

    /* Per-cluster L1 buffers (allocated on core 0 only) */
    void    *addr_a = 0;
    void    *addr_b = 0;
    uint32_t off_a_raw = 0, off_b_raw = 0;
    uint32_t off_a = 0, off_b = 0;
    void    *ptr_a = 0;
    void    *ptr_b = 0;

    if (core == 0) {
        /* add slack for 64B alignment */
        addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);

        if (addr_a == 0 || addr_b == 0) {
            /* fail early if L1 ran out (very unlikely here) */
            if (cid == 0) { flex_eoc(1); }
            return 0;
        }

        /* compute TCDM offsets; bump to 64B alignment for DMA engines */
        off_a_raw = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        off_b_raw = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        off_a     = align_up_u32(off_a_raw, 64u);
        off_b     = align_up_u32(off_b_raw, 64u);

        /* aligned pointers for CPU-side zeroing (don’t write through local(offset)) */
        ptr_a = (void *)((uintptr_t)addr_a + (uintptr_t)(off_a - off_a_raw));
        ptr_b = (void *)((uintptr_t)addr_b + (uintptr_t)(off_b - off_b_raw));

        zero_u32(ptr_a, BYTES_A_STRIP);
        zero_u32(ptr_b, BYTES_B_STRIP);
    }

    /* Ensure all clusters finished setup before DMA */
    flex_global_barrier_xy();

    if (core == 0) {
        /* -------- A strip: contiguous 1D load (64x256x4) -------- */
        const uint32_t hA = hbm_off_A_strip(cy);
        bare_dma_start_1d(/*dst*/ local(off_a), /*src*/ hbm_addr(hA), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();

        /* -------- B strip: 2D strided gather (256 rows × 64 cols) -------- */
        const uint32_t hB   = hbm_off_B_strip_base(cx);
        const uint32_t brow = (uint32_t)(TILE * ELEM_BYTES);    /* 64 * 4 = 256 bytes per row */
        const uint32_t dstr = brow;                             /* packed in L1 */
        const uint32_t sstr = (uint32_t)(MAT_N * ELEM_BYTES);   /* 256 * 4 = 1024 bytes */
        const uint32_t reps = (uint32_t)MAT_N;                  /* 256 rows */

        bare_dma_start_2d(/*dst*/ local(off_b), /*src*/ hbm_addr(hB),
                          /*bytes/row*/ brow,
                          /*dst_stride*/ dstr,
                          /*src_stride*/ sstr,
                          /*repeat*/ reps);
        bare_dma_wait_all();
    }

    /* Fence before exiting to avoid tearing */
    flex_global_barrier_xy();

    /* Single end-of-compute */
    if (cid == 0 && core == 0) { flex_eoc(0); }
    return 0;
}
