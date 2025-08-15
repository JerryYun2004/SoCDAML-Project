/* main_load_nonextend.c — measure direct HBM→L1 load time
 * - Each cluster’s DM core loads its own A (1D) and B (2D) strip.
 * - No prints, no checksum, no compute, no store.
 * - Uses your existing layout constants/helpers from fixed_proj.h.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "soc_daml.h"   /* for FlexPosition, get_pos(), tcdm_offset_from_ptr, ARCH_* */

/* ----- tiny local helpers (no std headers) ----- */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void*)v;
}

static inline void zero_u32(void *ptr, uint32_t bytes)
{
    volatile uint32_t *p = (volatile uint32_t *)ptr;
    const uint32_t n = bytes >> 2;
    for (uint32_t i = 0; i < n; ++i) { p[i] = 0u; }
}

/* one 1KB row write to HBM via DMA (used only by the optional init) */
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* ------------------- optional one-time HBM init (silent) -------------------
   Preloading matrices A & B into HBM.
   If your DRAM model already preloads A/B, you can comment this out.
   A: ramp (idx+1)/65536, B: identity with 2.0 on diag columns {0,64,128,192}. */
static void init_hbm_AB_silent(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

     /* convenience predicate: only C[0,0] DM emits timer stamps */
    const uint32_t is_timer_master = (uint32_t)((P.x == 0u) & (P.y == 0u));
    
    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * ELEM_BYTES; /* 256*4 = 1024 */
    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) { flex_eoc(1); return; }

    void *row_aln = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A rows */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            uint32_t idx = r * MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* B rows */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) pf[c] = 0.0f;
        if (r < MAT_N) {
            if ((r % C_TILE_COLS) == 0u) pf[r] = 2.0f;  /* columns 0,64,128,192 */
            else                          pf[r] = 1.0f;
        }
        const uint32_t offB_row = HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }
}

/* =============================== MAIN =============================== */
int main(void)
{
    /* bring-up and alloc in sync */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t IS_DM = flex_is_dm_core();

    /* (Optional) initialize HBM contents once from C[0,0] silently */
    init_hbm_AB_silent();
    flex_global_barrier_xy();

    /* convenience predicate: only C[0,0] DM emits timer stamps */
    const uint32_t is_timer_master = (uint32_t)((P.x == 0u) & (P.y == 0u));
    
    if (is_timer_master) { flex_timer_start(); } 
    /* -------- L1 alloc (DM core per cluster) -------- */
    void *addr_a = (void*)0, *addr_b = (void*)0;
    uint32_t a_off = 0u, b_off = 0u;

    if (IS_DM) {
        void *raw_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        void *raw_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
        if (!raw_a || !raw_b) { flex_eoc(1); return 1; }

        addr_a = align64(raw_a);
        addr_b = align64(raw_b);

        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);

        zero_u32(addr_a, BYTES_A_STRIP);
        zero_u32(addr_b, BYTES_B_STRIP);
    }
    if (is_timer_master) { flex_timer_end(); }    /* global stamp end (end of data part 1) */
    flex_global_barrier_xy();

    /* -------- DIRECT LOADS (parallel DMA, no prints) -------- */
    if (IS_DM) {
        /* A: contiguous strip for row P.y (1D) */
        const uint32_t offA = hbm_off_A_strip(P.y);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
        bare_dma_wait_all();

        /* B: gather 256 rows of 64 (2D) for column P.x */
        const uint32_t offB_base    = hbm_off_B_strip_base(P.x);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;
        const uint32_t src_stride   = (uint32_t)MAT_N * ELEM_BYTES;        /* 256*4 */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                          size_per_row, dst_stride, src_stride, (uint32_t)B_STRIP_ROWS);
        bare_dma_wait_all();
    }

    flex_global_barrier_xy();

    /* single end-of-compute to stop simulation */
    if (cid == 0u && core == 0u) { flex_eoc(0); }
    return 0;
}
