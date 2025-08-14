// main_direct_load.c
#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_printf.h"
#include "flex_dma_pattern.h"

#define MAT_N         (256u)
#define TILE          (64u)
#define ELEM_BYTES    (4u)
#define BYTES_A_STRIP (TILE * MAT_N * ELEM_BYTES)   /* 64x256 */
#define BYTES_B_STRIP (MAT_N * TILE * ELEM_BYTES)   /* 256x64 */

#define HBM_A_BASE    (0x00000400u)
#define HBM_B_BASE    (0x00040400u)

static inline uint32_t hbm_off_A_strip(uint32_t r) { return HBM_A_BASE + r * BYTES_A_STRIP; }
static inline uint32_t hbm_off_B_strip_base(uint32_t c) { return HBM_B_BASE + (c * TILE) * ELEM_BYTES; }

static inline uint32_t tcdm_off(void *p) { return ((uint32_t)(uintptr_t)p) - (uint32_t)ARCH_CLUSTER_TCDM_BASE; }
static inline uint32_t add32_sum(const void *ptr, uint32_t n_bytes) {
    const uint32_t *p = (const uint32_t*)ptr; uint32_t n = n_bytes >> 2, s = 0u;
    for (uint32_t i = 0; i < n; ++i) s += p[i]; return s;
}
static inline uint32_t rdcycle32(void){ uint32_t c; asm volatile("csrr %0, mcycle" : "=r"(c)); return c; }

int main(void)
{
    flex_barrier_xy_init();
    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const uint32_t nx   = flex_get_barrier_num_cluster_x();
    const uint32_t ny   = flex_get_barrier_num_cluster_y();
    const uint32_t cx   = cid % nx;
    const uint32_t cy   = cid / nx;

    if (cid == 0 && core == 0) {
        printf("[Info][NONEXT] HBM->L1 load test (no broadcast)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n", nx, ny, ARCH_NUM_CORE_PER_CLUSTER);
    }
    flex_global_barrier_xy();

    void *addr_a = 0, *addr_b = 0;
    if (flex_is_dm_core()) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP);
        addr_b = flex_l1_malloc(BYTES_B_STRIP);
        if (!addr_a || !addr_b) {
            printf("[ERR][C%u] L1 malloc failed A=%p B=%p\n", cid, addr_a, addr_b);
            flex_eoc(1); return 1;
        }
    }
    flex_global_barrier_xy();

    if (flex_is_dm_core()) {
        const uint32_t a_off = tcdm_off(addr_a);
        const uint32_t b_off = tcdm_off(addr_b);

        /* A: 1D contiguous copy for our row strip */
        const uint32_t a_h = hbm_off_A_strip(cy);
        uint32_t t0 = rdcycle32();
        bare_dma_start_1d(local(a_off), hbm_addr(a_h), BYTES_A_STRIP);
        bare_dma_wait_all();

        /* B: 2D copy for our column block */
        const uint32_t b_h = hbm_off_B_strip_base(cx);
        const size_t   size_per_row = TILE * ELEM_BYTES;
        const size_t   dst_stride   = TILE * ELEM_BYTES;
        const size_t   src_stride   = MAT_N * ELEM_BYTES;
        bare_dma_start_2d(local(b_off), hbm_addr(b_h), size_per_row, dst_stride, src_stride, MAT_N);
        bare_dma_wait_all();
        uint32_t t1 = rdcycle32();

        /* ordered print to avoid interleaving */
        for (uint32_t oy = 0; oy < ny; ++oy) {
            for (uint32_t ox = 0; ox < nx; ++ox) {
                flex_global_barrier_xy();
                if (cx == ox && cy == oy) {
                    printf("[Load][NONEXT] C[%u,%u] A.add=0x%08x B.add=0x%08x cycles=%u\n",
                           cy, cx, add32_sum(addr_a, BYTES_A_STRIP), add32_sum(addr_b, BYTES_B_STRIP), (unsigned)(t1 - t0));
                }
            }
        }
    }
    flex_global_barrier_xy();

    if (cid == 0 && core == 0) printf("Done (direct load test).\n");
    flex_eoc(0);
    return 0;
}
