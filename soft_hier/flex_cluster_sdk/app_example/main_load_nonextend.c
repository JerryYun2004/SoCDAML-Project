// main_load_direct.c
#include <stdint.h>
#include "fixed_proj.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"

// -------- cycle counter (portable for RV) --------
static inline uint64_t rdcycle64(void) {
    uint32_t hi1, lo, hi2;
    asm volatile ("rdcycleh %0" : "=r"(hi1));
    asm volatile ("rdcycle  %0" : "=r"(lo));
    asm volatile ("rdcycleh %0" : "=r"(hi2));
    if (hi1 != hi2) {
        asm volatile ("rdcycleh %0" : "=r"(hi1));
        asm volatile ("rdcycle  %0" : "=r"(lo));
        asm volatile ("rdcycleh %0" : "=r"(hi2));
    }
    return (((uint64_t)hi2) << 32) | lo;
}

__attribute__((section(".hbm"))) static volatile uint64_t g_cycles[ARCH_NUM_CLUSTER];

int main(void)
{
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    if (cid == 0 && core == 0) {
        printf("[Info][NONEXT] Timing per-cluster HBM loads (no broadcast)\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem_bytes=%u\n", MAT_N, TILE, ELEM_BYTES);
        printf("       A-strip bytes=%u, B-strip bytes=%u\n", BYTES_A_STRIP, BYTES_B_STRIP);
    }

    // ---- Allocate L1 buffers (DM core only) ----
    void *addr_a = 0, *addr_b = 0;
    if (core == 0) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP);
        addr_b = flex_l1_malloc(BYTES_B_STRIP);
        if (!addr_a || !addr_b) {
            printf("[ERR][NONEXT][C%u] L1 malloc failed A=%p B=%p\n", cid, addr_a, addr_b);
            flex_eoc(1);
            return 1;
        }
        zero_f32(addr_a, BYTES_A_STRIP);
        zero_f32(addr_b, BYTES_B_STRIP);
    }
    flex_global_barrier_xy();

    // Offsets (cluster-local)
    uint32_t a_off = 0, b_off = 0;
    if (core == 0) {
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    }
    flex_global_barrier_xy();

    // ---- Start timing: each cluster loads its own A and B from HBM ----
    uint64_t t0 = 0, t1 = 0;
    if (core == 0) t0 = rdcycle64();
    flex_global_barrier_xy();

    if (core == 0) {
        // A_r for our row r=P.y (1D contiguous)
        const uint32_t r = P.y;
        const uint32_t a_h = hbm_off_A_strip(r);
        bare_dma_start_1d(local(a_off), hbm_addr(a_h), BYTES_A_STRIP);
        bare_dma_wait_all();

        // B_c for our column c=P.x (2D strided)
        const uint32_t c = P.x;
        const uint32_t b_h = hbm_off_B_strip_base(c);
        const uint32_t size_per_row = (B_STRIP_COLS*ELEM_BYTES); // 64*4
        const uint32_t dst_stride   = (B_STRIP_COLS*ELEM_BYTES); // 64*4
        const uint32_t src_stride   = (MAT_N       *ELEM_BYTES); // 256*4
        const uint32_t repeat       = (B_STRIP_ROWS);            // 256
        bare_dma_start_2d(local(b_off), hbm_addr(b_h),
                          size_per_row, dst_stride, src_stride, repeat);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    if (core == 0) {
        t1 = rdcycle64();
        g_cycles[cid] = (t1 - t0);
    }
    flex_global_barrier_xy();

    // ---- Ordered printing so logs are readable ----
    if (cid == 0 && core == 0) {
        printf("[Result][NONEXT] per-cluster cycles to obtain A/B in L1 (all load from HBM):\n");
    }
    for (uint32_t ry = 0; ry < ARCH_NUM_CLUSTER_Y; ++ry) {
        for (uint32_t rx = 0; rx < ARCH_NUM_CLUSTER_X; ++rx) {
            flex_global_barrier_xy();
            if (core == 0 && P.x == rx && P.y == ry) {
                printf("  C[%u,%u] cycles=%llu  (A=%uB, B=%uB)\n",
                       rx, ry, (unsigned long long)g_cycles[cid],
                       (unsigned)BYTES_A_STRIP, (unsigned)BYTES_B_STRIP);
            }
            flex_global_barrier_xy();
        }
    }

    if (cid == 0 && core == 0) printf("[Done][NONEXT]\n");
    flex_eoc(0);
    return 0;
}
