/* main_direct.c — direct HBM→L1 loads without inter-cluster broadcast
 *
 * Measure HBM load cost when every cluster fetches its A/B strips directly.
 * Print order is strictly serialized to avoid interleaving on GVSoC console.
 * No <stdio.h>/<stddef.h>.
 */
#include <stdint.h>
#include "flex_runtime.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "soc_daml.h"

/* ----- tiny helpers (no libc) ----- */
static inline uint32_t align_up_u32(uint32_t v, uint32_t a)
{ return (v + a - 1u) & ~(a - 1u); }

static inline void zero_u32(void *dst, uint32_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)dst;
    for (uint32_t i = 0; i < (bytes >> 2); ++i) p[i] = 0u;
}

/* Sum of 32-bit words (debug fingerprint). */
static inline uint64_t add_u32_words(const void *ptr, uint32_t n_words)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint64_t acc = 0;
    for (uint32_t i = 0; i < n_words; ++i) acc += (uint64_t)p[i];
    return acc;
}

int main(void)
{
    flex_barrier_xy_init();
    flex_alloc_init();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);  /* P.x = col (0..3), P.y = row (0..3) */

    /* Header once */
    if (cid == 0 && core == 0) {
        printf("[Info][NONEXT] HBM->L1 direct load test (no broadcast)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               (unsigned)ARCH_NUM_CLUSTER_X, (unsigned)ARCH_NUM_CLUSTER_Y,
               (unsigned)ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem=%uB, A/B strip bytes=%u\n",
               (unsigned)MAT_N, (unsigned)TILE, (unsigned)ELEM_BYTES,
               (unsigned)BYTES_A_STRIP);
    }
    flex_global_barrier_xy();

    /* Allocate L1 per cluster (DM core only). */
    void *addr_a = 0; /* 64x256 */
    void *addr_b = 0; /* 256x64 */
    uint32_t a_off = 0, b_off = 0;

    if (core == 0) {
        /* 64B alignment for DMA. */
        addr_a = flex_l1_malloc(BYTES_A_STRIP + 64);
        addr_b = flex_l1_malloc(BYTES_B_STRIP + 64);
        if (!addr_a || !addr_b) {
            if (cid == 0) printf("[ERR] L1 malloc failed (A=%p, B=%p)\n", addr_a, addr_b);
            flex_eoc(1);
        }
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        a_off = align_up_u32(a_off, 64);
        b_off = align_up_u32(b_off, 64);

        /* Zero targets to keep prints deterministic. */
        zero_u32((void*)local(a_off), BYTES_A_STRIP);
        zero_u32((void*)local(b_off), BYTES_B_STRIP);
    }
    flex_global_barrier_xy();

    /* Each cluster loads its own A/B from HBM (DM core only). */
    uint64_t addA = 0, addB = 0;

    if (core == 0) {
        /* Compute HBM offsets for this cluster's tiles. */
        const uint32_t r = P.y; /* A row strip index 0..3 */
        const uint32_t c = P.x; /* B col strip index 0..3 */
        const uint32_t offA  = hbm_off_A_strip(r);
        const uint32_t baseB = hbm_off_B_strip_base(c);

        /* 1D DMA copy: A strip (64x256 contiguous) */
        bare_dma_start_1d(local(a_off), hbm_addr(offA), BYTES_A_STRIP);
        bare_dma_wait_all();
        addA = add_u32_words((void*)local(a_off), BYTES_A_STRIP >> 2);

        /* 2D DMA copy: B strip (256 rows of 64 cols) */
        const uint32_t sz_row  = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_str = sz_row;                               /* packed */
        const uint32_t src_str = (uint32_t)MAT_N * ELEM_BYTES;         /* 256*4 */
        const uint32_t reps    = (uint32_t)B_STRIP_ROWS;               /* 256   */
        bare_dma_start_2d(local(b_off), hbm_addr(baseB),
                          sz_row, dst_str, src_str, reps);
        bare_dma_wait_all();
        addB = add_u32_words((void*)local(b_off), BYTES_B_STRIP >> 2);
    }
    flex_global_barrier_xy();

    /* ----- STRICTLY ORDERED PRINTS (avoid interleaving) ----- */
    if (core == 0) {
        /* 1) Offsets (optional – useful to verify same layout everywhere) */
        for (uint32_t oy = 0; oy < ARCH_NUM_CLUSTER_Y; ++oy) {
            for (uint32_t ox = 0; ox < ARCH_NUM_CLUSTER_X; ++ox) {
                flex_global_barrier_xy();
                if (P.x == ox && P.y == oy) {
                    printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u\n",
                           (unsigned)P.x, (unsigned)P.y,
                           (unsigned)a_off, (unsigned)b_off);
                }
                flex_global_barrier_xy();
            }
        }

        /* 2) A/B checksums (one cluster at a time) */
        for (uint32_t oy = 0; oy < ARCH_NUM_CLUSTER_Y; ++oy) {
            for (uint32_t ox = 0; ox < ARCH_NUM_CLUSTER_X; ++ox) {
                flex_global_barrier_xy();
                if (P.x == ox && P.y == oy) {
                    printf("[Load][A][NONEXT] C[%u,%u](DM) add=0x%08x%08x\n",
                           (unsigned)P.x, (unsigned)P.y,
                           (unsigned)(addA >> 32), (unsigned)(addA & 0xffffffffu));
                    printf("[Load][B][NONEXT] C[%u,%u](DM) add=0x%08x%08x\n",
                           (unsigned)P.x, (unsigned)P.y,
                           (unsigned)(addB >> 32), (unsigned)(addB & 0xffffffffu));
                }
                flex_global_barrier_xy();
            }
        }
    }
    flex_global_barrier_xy();

    if (cid == 0 && core == 0) {
        printf("[DONE][NONEXT] HBM->L1 direct load test finished.\n");
    }
    flex_global_barrier_xy();

    flex_eoc(0);
    return 0;
}
