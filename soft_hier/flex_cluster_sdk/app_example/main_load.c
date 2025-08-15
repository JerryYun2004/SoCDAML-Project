/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 *
 * Timed sections (console prints at the very end):
 *   - Data transfer cycles: L1 alloc + leaders' HBM loads + broadcasts (+ waits)
 *   - Synchronization cycles: the two global barriers between/after those phases
 *
 * Functional behavior is unchanged.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"
#include "flex_printf.h"   /* lightweight printf using flex_log_char */

/* ---- tiny helpers (no std headers beyond stdint.h) ---- */

static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void *)v;
}

static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* Safe 32-bit cycle read (no cycleh, avoids CSR warnings) */
static inline uint32_t rdcycle32(void)
{
    uint32_t c;
    asm volatile ("csrr %0, cycle" : "=r"(c));
    return c;
}

/* fctprintf adapter that routes characters to the platform logger */
static void fp_out_char(char c, void *arg)
{
    (void)arg;
    flex_log_char(c);
}
#define FPRINTF(...)  fctprintf(fp_out_char, 0, __VA_ARGS__)

/* ------------------- one-time HBM init (silent, same as main_direct) -------------------
   A: ramp (idx+1)/65536;  B: identity with 2.0 on diag columns {0,64,128,192}. */
static void init_hbm_AB_silent(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;  /* only C[0,0] DM core does this */

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES; /* 256*4 = 1024 */

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) { flex_eoc(1); return; }

    void *row_aln       = align64(row_raw);
    const uint32_t off  = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A rows: ramp (idx+1)/65536 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) {
            uint32_t idx = r * (uint32_t)MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        dma_write_row((uint32_t)HBM_A_BASE_OFFSET + r * ROW_BYTES, off, ROW_BYTES);
    }

    /* B rows: identity; diag=2.0 for columns 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        pf[r] = ((r % (uint32_t)C_TILE_COLS) == 0u) ? 2.0f : 1.0f;
        dma_write_row((uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES, off, ROW_BYTES);
    }
}

/* =================================== MAIN =================================== */
int main(void)
{
    /* Bring-up (excluded from timing) */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    /* Silent HBM preload (excluded from timing) */
    init_hbm_AB_silent();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before any load */

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */

    /* Only DM/core0 touches DMA/L1 in this microbench */
    if (core != 0u) { flex_eoc(0); return 0; }

    /* Timers (software visible) */
    uint32_t cycles_data = 0;
    uint32_t cycles_sync = 0;
    uint32_t t0;

    /* ---------------- Data: L1 alloc + HBM pulls (leaders) ---------------- */
    flex_timer_start();                     /* sim hook begin (data part 1)  */
    t0 = rdcycle32();

    /* L1 allocations (per-cluster) */
    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    /* 64B-aligned TCDM offsets */
    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Leaders pull from HBM */
    if (P.x == 0u) {
        const uint32_t r = P.y;
        bare_dma_start_1d(local(a_off), hbm_addr(hbm_off_A_strip(r)), BYTES_A_STRIP);
        bare_dma_wait_all();
    }
    if (P.y == 0u) {
        const uint32_t c           = P.x;
        const uint32_t base        = hbm_off_B_strip_base(c);
        const uint32_t size_per_row= (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES;
        const uint32_t dst_stride  = size_per_row;
        const uint32_t src_stride  = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;
        const uint32_t repeat      = (uint32_t)B_STRIP_ROWS;
        bare_dma_start_2d(local(b_off), hbm_addr(base),
                          size_per_row, dst_stride, src_stride, repeat);
        bare_dma_wait_all();
    }

    cycles_data += (uint32_t)(rdcycle32() - t0);
    flex_timer_end();                       /* sim hook end   (data part 1)  */

    /* ---------------- Sync: fence before broadcasts ---------------- */
    t0 = rdcycle32();
    flex_global_barrier_xy();
    cycles_sync += (uint32_t)(rdcycle32() - t0);

    /* ---------------- Data: broadcasts (+ waits) ------------------- */
    flex_timer_start();                     /* sim hook begin (data part 2)  */
    t0 = rdcycle32();

    if (P.x == 0u) {
        flex_dma_async_broadcast(a_off, a_off, BYTES_A_STRIP,
                                 mask_row(P.y), mask_all4());
    }
    if (P.y == 0u) {
        flex_dma_async_broadcast(b_off, b_off, BYTES_B_STRIP,
                                 mask_all4(),   mask_col(P.x));
    }
    flex_dma_async_wait_all();  /* count this as data, not sync */

    cycles_data += (uint32_t)(rdcycle32() - t0);
    flex_timer_end();                       /* sim hook end   (data part 2)  */

    /* ---------------- Sync: final fence (everyone has strips) ------ */
    t0 = rdcycle32();
    flex_global_barrier_xy();
    cycles_sync += (uint32_t)(rdcycle32() - t0);

    /* ---------------- Print once from C[0,0] DM core ---------------- */
    if (P.x == 0u && P.y == 0u && core == 0u) {
        FPRINTF("[Timing][BCAST] data_transfer_cycles=%u\n", (unsigned)cycles_data);
        FPRINTF("[Timing][BCAST] synchronization_cycles=%u\n",  (unsigned)cycles_sync);
    }

    flex_eoc(0);
    return 0;
}
