/* main_load.c — HBM→L1 load tests with or without inter-cluster broadcast
 *
 * Toggle:
 *   #define USE_BROADCAST 1  // leaders load A/B then broadcast to row/col
 *   #define USE_BROADCAST 0  // every cluster loads its own A/B directly
 *
 * No <stdio.h>/<stddef.h>. Uses SDK printf and barriers.
 */

#define USE_BROADCAST 1

#include <stdint.h>
#include "flex_runtime.h"       /* cluster/core ids, barriers, get_pos(), local(), hbm_addr() */
#include "flex_dma_pattern.h"   /* bare_dma_* and flex_dma_async_broadcast */
#include "fixed_proj.h"         /* MAT_N, TILE, ELEM_BYTES, BYTES_A_STRIP, BYTES_B_STRIP, masks & HBM offs */
#include "soc_daml.h"           /* flex_alloc_init, flex_l1_malloc, tcdm_offset_from_ptr, flex_eoc */

/* -------------------- Local helpers (unique names) -------------------- */
static inline uint32_t util_align_up_u32(uint32_t v, uint32_t a)
{ return (v + a - 1u) & ~(a - 1u); }

static inline void util_zero_u32(void *dst, uint32_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)dst;
    for (uint32_t i = 0; i < (bytes >> 2); ++i) p[i] = 0u;
}

static inline uint64_t util_add_u32_words(const void *ptr, uint32_t n_words)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n_words; ++i) acc += (uint64_t)p[i];
    return acc;
}

static inline uint32_t util_tcdm_off(void *p)
{
    return tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, p);
}

/* ------------- one-cluster-at-a-time printing -------------- */
static void print_offsets_ordered(uint32_t cx, uint32_t cy,
                                  uint32_t a_off, uint32_t b_off)
{
    for (uint32_t oy = 0; oy < ARCH_NUM_CLUSTER_Y; ++oy) {
        for (uint32_t ox = 0; ox < ARCH_NUM_CLUSTER_X; ++ox) {
            flex_global_barrier_xy();
            if (cx == ox && cy == oy) {
                printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u\n",
                       (unsigned)cx, (unsigned)cy,
                       (unsigned)a_off, (unsigned)b_off);
            }
            flex_global_barrier_xy();
        }
    }
}

static void print_adds_ordered(uint32_t cx, uint32_t cy,
                               uint64_t addA, uint64_t addB,
                               const char *tagA, const char *tagB)
{
    for (uint32_t oy = 0; oy < ARCH_NUM_CLUSTER_Y; ++oy) {
        for (uint32_t ox = 0; ox < ARCH_NUM_CLUSTER_X; ++ox) {
            flex_global_barrier_xy();
            if (cx == ox && cy == oy) {
                printf("%s C[%u,%u](DM) add=0x%08x%08x\n",
                       tagA, (unsigned)cx, (unsigned)cy,
                       (unsigned)(addA >> 32), (unsigned)(addA & 0xffffffffu));
                printf("%s C[%u,%u](DM) add=0x%08x%08x\n",
                       tagB, (unsigned)cx, (unsigned)cy,
                       (unsigned)(addB >> 32), (unsigned)(addB & 0xffffffffu));
            }
            flex_global_barrier_xy();
        }
    }
}

int main(void)
{
    flex_barrier_xy_init();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t cx = P.x;  /* 0..3 */
    const uint32_t cy = P.y;  /* 0..3 */

#if USE_BROADCAST
    if (cid == 0 && core == 0) {
        printf("[Info][BCAST] HBM->L1 load test with row/col broadcast\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               (unsigned)ARCH_NUM_CLUSTER_X, (unsigned)ARCH_NUM_CLUSTER_Y,
               (unsigned)ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem=%uB, A/B strip bytes=%u\n",
               (unsigned)MAT_N, (unsigned)TILE, (unsigned)ELEM_BYTES,
               (unsigned)BYTES_A_STRIP);
    }
#else
    if (cid == 0 && core == 0) {
        printf("[Info][NONEXT] HBM->L1 direct load test (no broadcast)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               (unsigned)ARCH_NUM_CLUSTER_X, (unsigned)ARCH_NUM_CLUSTER_Y,
               (unsigned)ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem=%uB, A/B strip bytes=%u\n",
               (unsigned)MAT_N, (unsigned)TILE, (unsigned)ELEM_BYTES,
               (unsigned)BYTES_A_STRIP);
    }
#endif
    flex_global_barrier_xy();

    /* Allocate per-cluster L1 (DM core only). */
    void *addr_a = 0, *addr_b = 0;
    uint32_t a_off = 0, b_off = 0;

    if (core == 0) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
        if (!addr_a || !addr_b) {
            if (cid == 0) printf("[ERR] L1 malloc failed (A=%p, B=%p)\n", addr_a, addr_b);
            flex_eoc(1);
        }
        a_off = util_tcdm_off(addr_a);
        b_off = util_tcdm_off(addr_b);
        a_off = util_align_up_u32(a_off, 64u);
        b_off = util_align_up_u32(b_off, 64u);

        util_zero_u32((void*)local(a_off), BYTES_A_STRIP);
        util_zero_u32((void*)local(b_off), BYTES_B_STRIP);
    }
    flex_global_barrier_xy();

#if USE_BROADCAST
    /* ---------------- Leaders load from HBM ---------------- */
    if (core == 0) {
        if (cx == 0u) {
            const uint32_t h_off = hbm_off_A_strip(cy);   /* from fixed_proj.h */
            bare_dma_start_1d(local(a_off), hbm_addr(h_off), BYTES_A_STRIP);
            bare_dma_wait_all();
            /* small, post-DMA print */
            printf("[Load][A][BCAST] C[%u,0](DM) off=0x%08x bytes=%u\n",
                   (unsigned)cy, (unsigned)h_off, (unsigned)BYTES_A_STRIP);
        }
        if (cy == 0u) {
            const uint32_t h_off = hbm_off_B_strip_base(cx); /* from fixed_proj.h */
            const uint32_t size_per_row = TILE * ELEM_BYTES;   /* 64*4 */
            const uint32_t dst_stride   = size_per_row;
            const uint32_t src_stride   = MAT_N * ELEM_BYTES;  /* 256*4 */
            bare_dma_start_2d(local(b_off), hbm_addr(h_off),
                              size_per_row, dst_stride, src_stride, MAT_N);
            bare_dma_wait_all();
            printf("[Load][B][BCAST] C[0,%u](DM) base=0x%08x rows=%u\n",
                   (unsigned)cx, (unsigned)h_off, (unsigned)MAT_N);
        }
    }
    flex_global_barrier_xy();

    /* ---------------- Row/Col broadcast -------------------- */
    if (core == 0) {
        if (cx == 0u) {
            /* masks from fixed_proj.h: mask_row(row), mask_all4() */
            flex_dma_async_broadcast(a_off, a_off, BYTES_A_STRIP,
                                     mask_row(cy), mask_all4());
        }
        if (cy == 0u) {
            /* masks from fixed_proj.h: mask_col(col) */
            flex_dma_async_broadcast(b_off, b_off, BYTES_B_STRIP,
                                     mask_all4(), mask_col(cx));
        }
        flex_dma_async_wait_all();
    }
    flex_global_barrier_xy();

    /* ---------------- Verify (ordered prints) --------------- */
    if (core == 0) {
        const uint64_t addA = util_add_u32_words((void*)local(a_off), BYTES_A_STRIP >> 2);
        const uint64_t addB = util_add_u32_words((void*)local(b_off), BYTES_B_STRIP >> 2);
        print_offsets_ordered(cx, cy, a_off, b_off);
        print_adds_ordered(cx, cy, addA, addB, "[Verify][A][BCAST]", "[Verify][B][BCAST]");
    }

#else  /* -------------------- DIRECT LOAD (no broadcast) -------------------- */

    if (core == 0) {
        const uint32_t offA  = hbm_off_A_strip(cy);
        const uint32_t baseB = hbm_off_B_strip_base(cx);

        /* A: contiguous 1D */
        bare_dma_start_1d(local(a_off), hbm_addr(offA), BYTES_A_STRIP);
        bare_dma_wait_all();

        /* B: 2D rows */
        const uint32_t size_per_row = TILE * ELEM_BYTES;   /* 64*4 */
        const uint32_t dst_stride   = size_per_row;
        const uint32_t src_stride   = MAT_N * ELEM_BYTES;  /* 256*4 */
        bare_dma_start_2d(local(b_off), hbm_addr(baseB),
                          size_per_row, dst_stride, src_stride, MAT_N);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    /* Verify (ordered) */
    if (core == 0) {
        const uint64_t addA = util_add_u32_words((void*)local(a_off), BYTES_A_STRIP >> 2);
        const uint64_t addB = util_add_u32_words((void*)local(b_off), BYTES_B_STRIP >> 2);
        print_offsets_ordered(cx, cy, a_off, b_off);
        print_adds_ordered(cx, cy, addA, addB, "[Load][A][NONEXT]", "[Load][B][NONEXT]");
    }

#endif /* USE_BROADCAST */

    if (cid == 0 && core == 0) {
#if USE_BROADCAST
        printf("[DONE][BCAST] HBM->L1 broadcast load test finished.\n");
#else
        printf("[DONE][NONEXT] HBM->L1 direct load test finished.\n");
#endif
    }
    flex_global_barrier_xy();

    flex_eoc(0);
    return 0;
}
