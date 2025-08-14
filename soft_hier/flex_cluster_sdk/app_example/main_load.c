// main_broadcast_load.c
#include <stdint.h>
#include "flex_runtime.h"       // cluster/core id, barriers, local()/hbm_addr(), etc.  :contentReference[oaicite:0]{index=0}
#include "flex_alloc.h"         // flex_alloc_init, flex_l1_malloc                 :contentReference[oaicite:1]{index=1}
#include "flex_printf.h"
#include "flex_dma_pattern.h"   // bare/flex DMA + broadcast                       :contentReference[oaicite:2]{index=2}

/* ------------ Problem geometry (keep identical across variants) ------------- */
#define MAT_N         (256u)
#define TILE          (64u)
#define ELEM_BYTES    (4u)                     /* float */
#define BYTES_A_STRIP (TILE * MAT_N * ELEM_BYTES)   /* 64x256 */
#define BYTES_B_STRIP (MAT_N * TILE * ELEM_BYTES)   /* 256x64 */

/* ------------ HBM layout (same as your logs) -------------------------------- */
#define HBM_A_BASE    (0x00000400u)  /* A[0:64, 0:256] starts here; next row strip +65536 */
#define HBM_B_BASE    (0x00040400u)  /* B[:, 0] starts here; for col-block c add 0x100*c   */

static inline uint32_t hbm_off_A_strip(uint32_t r) {
    return HBM_A_BASE + r * BYTES_A_STRIP;
}
static inline uint32_t hbm_off_B_strip_base(uint32_t c) {
    /* row-major: column 0 at +0x000; column 64 at +0x100, etc. */
    return HBM_B_BASE + (c * TILE) * ELEM_BYTES;
}

/* ------------ Broadcast masks for a 4x4 cluster grid ------------------------ */
static inline uint16_t row_mask_4(uint32_t row) { return (uint16_t)(1u << row); }
static inline uint16_t col_mask_4(uint32_t col) { return (uint16_t)(1u << col); }
static inline uint16_t all4_mask(void)         { return (uint16_t)0x000Fu; }

/* ------------ Helper: TCDM pointer -> offset -------------------------------- */
static inline uint32_t tcdm_off(void *p) { return ((uint32_t)(uintptr_t)p) - (uint32_t)ARCH_CLUSTER_TCDM_BASE; }

/* ------------ Helper: cheap additive checksum (32b words) ------------------- */
static inline uint32_t add32_sum(const void *ptr, uint32_t n_bytes) {
    const uint32_t *p = (const uint32_t*)ptr;
    uint32_t n = n_bytes >> 2, s = 0u;
    for (uint32_t i = 0; i < n; ++i) s += p[i];
    return s;
}

/* ------------ Helper: read cycle counter (32-bit is fine for these tests) --- */
static inline uint32_t rdcycle32(void) { uint32_t c; asm volatile("csrr %0, mcycle" : "=r"(c)); return c; }

int main(void)
{
    /* bring up global XY barrier and allocators (once) */
    flex_barrier_xy_init();                      // grid-level sync bootstrap        :contentReference[oaicite:3]{index=3}
    flex_alloc_init();                           // init L1 & HBM allocators         :contentReference[oaicite:4]{index=4}
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const uint32_t nx   = flex_get_barrier_num_cluster_x();
    const uint32_t ny   = flex_get_barrier_num_cluster_y();
    const uint32_t cx   = cid % nx;
    const uint32_t cy   = cid / nx;

    if (cid == 0 && core == 0) {
        printf("[Info][BCAST] HBM->L1 load test with row/col broadcast\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n", nx, ny, ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem=%uB, A/B strip bytes=%u\n", MAT_N, TILE, ELEM_BYTES, BYTES_A_STRIP);
    }
    flex_global_barrier_xy();

    /* allocate same L1 layout on every cluster (DM core only) */
    void *addr_a = 0, *addr_b = 0;
    if (flex_is_dm_core()) {
        addr_a = flex_l1_malloc(BYTES_A_STRIP);
        addr_b = flex_l1_malloc(BYTES_B_STRIP);
        if (!addr_a || !addr_b) {
            printf("[ERR][C%u] L1 malloc failed A=%p B=%p\n", cid, addr_a, addr_b);
            flex_eoc(1);
            return 1;
        }
    }
    flex_global_barrier_xy();

    /* compute identical per-cluster TCDM offsets (DM core only) */
    uint32_t a_off = 0, b_off = 0;
    if (flex_is_dm_core()) {
        a_off = tcdm_off(addr_a);
        b_off = tcdm_off(addr_b);
        printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u\n", cx, cy, a_off, b_off);
    }
    flex_global_barrier_xy();

    /* ------------------ leaders pull from HBM ------------------ */
    uint32_t t_load_start = 0, t_load_end = 0;
    if (flex_is_dm_core()) {
        /* row leaders (x==0) load A-strip of their row */
        if (cx == 0u) {
            const uint32_t h_off = hbm_off_A_strip(cy);
            t_load_start = rdcycle32();
            bare_dma_start_1d(local(a_off), hbm_addr(h_off), BYTES_A_STRIP); /* 1D contiguous */  // :contentReference[oaicite:5]{index=5}
            bare_dma_wait_all();                                                                 // :contentReference[oaicite:6]{index=6}
            t_load_end = rdcycle32();
            printf("[Load][A][BCAST] C[%u,0](DM) off=0x%08x bytes=%u | add=0x%08x\n",
                   cy, h_off, BYTES_A_STRIP, add32_sum(addr_a, BYTES_A_STRIP));
        }

        /* column leaders (y==0) load B-strip of their column block with 2D */
        if (cy == 0u) {
            const uint32_t h_off = hbm_off_B_strip_base(cx);
            const size_t   size_per_row = TILE * ELEM_BYTES;   /* 64 * 4 */
            const size_t   dst_stride   = TILE * ELEM_BYTES;   /* contiguous rows in L1 */
            const size_t   src_stride   = MAT_N * ELEM_BYTES;  /* 256 * 4 */
            t_load_start = rdcycle32();
            bare_dma_start_2d(local(b_off), hbm_addr(h_off), size_per_row, dst_stride, src_stride, MAT_N); // :contentReference[oaicite:7]{index=7}
            bare_dma_wait_all();
            t_load_end = rdcycle32();
            printf("[Load][B][BCAST] C[0,%u](DM) base=0x%08x rows=%u | add=0x%08x\n",
                   cx, h_off, MAT_N, add32_sum(addr_b, BYTES_B_STRIP));
        }
    }
    flex_global_barrier_xy();

    /* ------------------ broadcast to row / column ------------------ */
    if (flex_is_dm_core()) {
        if (cx == 0u) {
            /* broadcast A along the row cy */
            flex_dma_async_broadcast(a_off, a_off, BYTES_A_STRIP, row_mask_4(cy), all4_mask());  // :contentReference[oaicite:8]{index=8}
        }
        if (cy == 0u) {
            /* broadcast B along the column cx */
            flex_dma_async_broadcast(b_off, b_off, BYTES_B_STRIP, all4_mask(), col_mask_4(cx));  // :contentReference[oaicite:9]{index=9}
        }
        flex_dma_async_wait_all();                                                               // :contentReference[oaicite:10]{index=10}
    }
    flex_global_barrier_xy();

    /* ------------------ verify on every cluster ------------------ */
    if (flex_is_dm_core()) {
        const uint32_t sa = add32_sum(addr_a, BYTES_A_STRIP);
        const uint32_t sb = add32_sum(addr_b, BYTES_B_STRIP);
        /* ordered print to avoid interleaving */
        for (uint32_t oy = 0; oy < ny; ++oy) {
            for (uint32_t ox = 0; ox < nx; ++ox) {
                flex_global_barrier_xy();
                if (cx == ox && cy == oy) {
                    printf("[Verify][BCAST] C[%u,%u] A.add=0x%08x  |  B.add=0x%08x\n", cy, cx, sa, sb);
                }
            }
        }
    }
    flex_global_barrier_xy();

    if (cid == 0 && core == 0) printf("Done (broadcast load test).\n");
    flex_eoc(0);
    return 0;
}
