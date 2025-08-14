// main_load_bcast.c
#include <stdint.h>
#include "fixed_proj.h"          // Sizes, HBM offset helpers, masks, zero_f32, etc.
#include "flex_runtime.h"        // Barriers, pos helpers, local()/hbm_addr(), etc.
#include "flex_alloc.h"
#include "flex_dma_pattern.h"    // bare_dma_start_{1d,2d}, flex_dma_async_broadcast, wait

// -------- cycle counter (portable for RV) --------
static inline uint64_t rdcycle64(void) {
    uint32_t hi1, lo, hi2;
    asm volatile ("rdcycleh %0" : "=r"(hi1));
    asm volatile ("rdcycle  %0" : "=r"(lo));
    asm volatile ("rdcycleh %0" : "=r"(hi2));
    if (hi1 != hi2) { // wrap during read; try once more
        asm volatile ("rdcycleh %0" : "=r"(hi1));
        asm volatile ("rdcycle  %0" : "=r"(lo));
        asm volatile ("rdcycleh %0" : "=r"(hi2));
    }
    return (((uint64_t)hi2) << 32) | lo;
}

// HBM-visible array to store per-cluster cycle deltas (for ordered printing)
__attribute__((section(".hbm"))) static volatile uint64_t g_cycles[ARCH_NUM_CLUSTER];

int main(void)
{
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);

    if (cid == 0 && core == 0) {
        printf("[Info][BCAST] Timing HBM→leaders + inter-cluster broadcast to all clusters\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem_bytes=%u\n", MAT_N, TILE, ELEM_BYTES);
        printf("       A-strip bytes=%u, B-strip bytes=%u\n", BYTES_A_STRIP, BYTES_B_STRIP);
    }

    // ---- Allocate L1 buffers on EVERY cluster (DM core only) ----
    void *addr_a = 0, *addr_b = 0;
    if (core == 0) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP);
        addr_b = flex_l1_malloc(BYTES_B_STRIP);
        if (!addr_a || !addr_b) {
            printf("[ERR][BCAST][C%u] L1 malloc failed A=%p B=%p\n", cid, addr_a, addr_b);
            flex_eoc(1);
            return 1;
        }
        zero_f32(addr_a, BYTES_A_STRIP);
        zero_f32(addr_b, BYTES_B_STRIP);
    }
    flex_global_barrier_xy();

    // Compute cluster-local offsets (identical across clusters by construction)
    uint32_t a_off = 0, b_off = 0;
    if (core == 0) {
        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    }
    flex_global_barrier_xy();

    // ---- Start timing: leaders load A/B from HBM, then broadcast to all ----
    uint64_t t0 = 0, t1 = 0;
    if (core == 0) t0 = rdcycle64();
    flex_global_barrier_xy(); // align start across clusters

    if (core == 0) {
        // Row leader (x==0) owns A_r for row r=y: 1D contiguous
        if (P.x == 0u) {
            const uint32_t r = P.y;
            const uint32_t h_off = hbm_off_A_strip(r);
            bare_dma_start_1d(local(a_off), hbm_addr(h_off), BYTES_A_STRIP);
            bare_dma_wait_all();
        }
        // Column leader (y==0) owns B_c for column c=x: 2D strided
        if (P.y == 0u) {
            const uint32_t c = P.x;
            const uint32_t h_off = hbm_off_B_strip_base(c);
            const uint32_t size_per_row = (B_STRIP_COLS*ELEM_BYTES); // 64*4
            const uint32_t dst_stride   = (B_STRIP_COLS*ELEM_BYTES); // 64*4
            const uint32_t src_stride   = (MAT_N       *ELEM_BYTES); // 256*4
            const uint32_t repeat       = (B_STRIP_ROWS);            // 256
            bare_dma_start_2d(local(b_off), hbm_addr(h_off),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
        }
    }
    flex_global_barrier_xy(); // everyone waits for leaders to finish HBM loads

    if (core == 0) {
        // Broadcast A along the row from (x=0,y=r) to all x in that row
        if (P.x == 0u) {
            const uint16_t row_m = (uint16_t)(1u << P.y);
            const uint16_t col_m = 0x000Fu; // all 4 columns
            flex_dma_async_broadcast(a_off, a_off, BYTES_A_STRIP, row_m, col_m);
        }
        // Broadcast B along the column from (x=c,y=0) to all y in that column
        if (P.y == 0u) {
            const uint16_t row_m = 0x000Fu; // all 4 rows
            const uint16_t col_m = (uint16_t)(1u << P.x);
            flex_dma_async_broadcast(b_off, b_off, BYTES_B_STRIP, row_m, col_m);
        }
        flex_dma_async_wait_all();
    }
    flex_global_barrier_xy(); // broadcast complete

    if (core == 0) {
        t1 = rdcycle64();
        g_cycles[cid] = (t1 - t0);
    }
    flex_global_barrier_xy();

    // ---- Ordered, clean printing of per-cluster times ----
    if (cid == 0 && core == 0) {
        printf("[Result][BCAST] per-cluster cycles to obtain A/B in L1 (HBM leaders + broadcast):\n");
    }
    for (uint32_t ry = 0; ry < ARCH_NUM_CLUSTER_Y; ++ry) {
        for (uint32_t rx = 0; rx < ARCH_NUM_CLUSTER_X; ++rx) {
            flex_global_barrier_xy();
            if (core == 0 && P.x == rx && P.y == ry) {
                // note: two strips resident per cluster: A_strip + B_strip
                printf("  C[%u,%u] cycles=%llu  (A=%uB, B=%uB)\n",
                       rx, ry, (unsigned long long)g_cycles[cid],
                       (unsigned)BYTES_A_STRIP, (unsigned)BYTES_B_STRIP);
            }
            flex_global_barrier_xy();
        }
    }

    if (cid == 0 && core == 0) printf("[Done][BCAST]\n");
    flex_eoc(0);
    return 0;
}
