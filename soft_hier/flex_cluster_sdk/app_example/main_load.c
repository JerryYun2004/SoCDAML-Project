/* main_load.c — Load-only microbenchmark (broadcast + fair HBM preload)
 *
 * Measures two sections (excluding preload time):
 *   - data_transfer_cycles: L1 allocations + leader HBM loads + broadcasts
 *   - synchronization_cycles: the two global barriers (after loads, after broadcasts)
 *
 * Functionally identical to your original load-only path.
 * Printing uses flex_printf.h's snprintf + flex_print(char*).
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"    /* MAT_N, ELEM_BYTES, C_TILE_COLS, BYTES_*,
                              HBM_*_BASE_OFFSET, mask_row/col/all4, hbm_off_*,
                              align_up_u32, local(), hbm_addr(), etc. */
#include "flex_printf.h"   /* provides snprintf(...) without std headers */

/* --- tiny helpers (no std headers) --- */
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

/* 32-bit cycle reader (avoid cycleh CSR to prevent warnings) */
static inline uint32_t rdcycle32(void)
{
    uint32_t v;
    asm volatile ("csrr %0, cycle" : "=r"(v));
    return v;
}
static inline uint32_t cycdelta(uint32_t start, uint32_t end)
{
    return (uint32_t)(end - start);  /* wraps naturally on 32-bit */
}

/* Minimal print helper using the provided snprintf + flex_print */
static inline void print_u(const char *fmt, uint32_t v)
{
    char buf[96];
    /* flex_printf.h provides snprintf(...) */
    (void)snprintf(buf, (size_t)sizeof(buf), fmt, v);
    flex_print(buf);
}

/* ------------------- one-time HBM init (silent, identical to main_direct) -------------------
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

    void *row_aln   = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A rows: ramp (idx+1)/65536 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) {
            uint32_t idx = r * (uint32_t)MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = (uint32_t)HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* B rows: identity; diag=2.0 for columns 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        if ((r % (uint32_t)C_TILE_COLS) == 0u) pf[r] = 2.0f;  /* 0,64,128,192 */
        else                                    pf[r] = 1.0f;
        const uint32_t offB_row = (uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }
}

/* =================================== MAIN =================================== */
int main(void)
{
    /* Bring-up / alloc */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    flex_alloc_init();
    flex_global_barrier_xy();

    /* Silent HBM preload (only C[0,0] DM executes). This time is EXCLUDED from measurements. */
    init_hbm_AB_silent();
    flex_global_barrier_xy();   /* ensure A/B are in HBM before any load */

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* P.x in [0..3], P.y in [0..3] */

    /* Only the DM core in each cluster should touch DMA/L1 in this microbench */
    if (core != 0u) {
        flex_eoc(0);
        return 0;
    }

    /* ===== Timing accumulators (32-bit cycles) =====
       We split into:
         - data_transfer_cycles: L1 allocs + HBM loads + broadcasts (no barriers)
         - synchronization_cycles: barrier after loads + barrier after broadcasts
    */
    uint32_t cycles_data = 0u;
    uint32_t cycles_sync = 0u;

    /* -------- L1 allocations (DM core per cluster) -------- */
    uint32_t t0 = rdcycle32();  /* begin data (part 1) */

    void *addr_a = flex_l1_malloc(BYTES_A_STRIP + 64u);  /* +64 for 64B alignment margin */
    void *addr_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
    if (addr_a == 0 || addr_b == 0) { flex_eoc(1); return 0; }

    /* Compute 64B-aligned TCDM offsets for DMA & broadcast engines */
    uint32_t a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
    uint32_t b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
    a_off = align_up_u32(a_off, 64u);
    b_off = align_up_u32(b_off, 64u);

    /* Optional clears (keep commented for pure load timing)
       zero_f32((void*)local(a_off), BYTES_A_STRIP);
       zero_f32((void*)local(b_off), BYTES_B_STRIP);
    */

    /* -------- Leaders pull from HBM (still in data section) -------- */
    /* A-strip: row leader (x==0) pulls its row’s 64x256 (1D contiguous). */
    if (P.x == 0u) {
        const uint32_t r = P.y;  /* 0..3 */
        const uint32_t h_off = hbm_off_A_strip(r);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(h_off), /*bytes*/ BYTES_A_STRIP);
        bare_dma_wait_all();
    }

    /* B-strip: column leader (y==0) pulls its column’s 256x64 (2D strided). */
    if (P.y == 0u) {
        const uint32_t c = P.x;  /* 0..3 */
        const uint32_t base         = hbm_off_B_strip_base(c);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;                                   /* packed */
        const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;         /* 256*4  */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                          /* 256    */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(base),
                          /*rowSize*/ size_per_row, /*dstStride*/ dst_stride,
                          /*srcStride*/ src_stride, /*rows*/ repeat);
        bare_dma_wait_all();
    }

    uint32_t t1 = rdcycle32();                 /* end data (part 1) */
    cycles_data += cycdelta(t0, t1);

    /* -------- Synchronize: all leaders must have finished HBM pulls -------- */
    t0 = rdcycle32();                          /* begin sync #1 */
    flex_global_barrier_xy();
    t1 = rdcycle32();                          /* end sync #1   */
    cycles_sync += cycdelta(t0, t1);

    /* -------- Inter-cluster broadcast (data section continues) -------- */
    t0 = rdcycle32();                          /* begin data (part 2) */

    /* Broadcast A along rows */
    if (P.x == 0u) {
        const uint16_t row_m = mask_row(P.y);  /* select row P.y */
        const uint16_t col_m = mask_all4();    /* all columns     */
        flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                 /*bytes*/ BYTES_A_STRIP, row_m, col_m);
    }

    /* Broadcast B down columns */
    if (P.y == 0u) {
        const uint16_t row_m = mask_all4();    /* all rows       */
        const uint16_t col_m = mask_col(P.x);  /* select col P.x */
        flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                 /*bytes*/ BYTES_B_STRIP, row_m, col_m);
    }

    /* Wait for any broadcast(s) triggered by this cluster */
    flex_dma_async_wait_all();

    t1 = rdcycle32();                          /* end data (part 2) */
    cycles_data += cycdelta(t0, t1);

    /* -------- Final sync to ensure every cluster has both strips -------- */
    t0 = rdcycle32();                          /* begin sync #2 */
    flex_global_barrier_xy();
    t1 = rdcycle32();                          /* end sync #2   */
    cycles_sync += cycdelta(t0, t1);

    /* -------- Print timing once (C[0,0], core 0 only) -------- */
    if (flex_is_dm_core()) {
        const uint32_t cid_here = flex_get_cluster_id();
        const FlexPosition P2   = get_pos(cid_here);
        if (P2.x == 0u && P2.y == 0u) {
            print_u("[Timing] data_transfer_cycles=%u\n", cycles_data);
            print_u("[Timing] synchronization_cycles=%u\n", cycles_sync);
        }
    }

    flex_eoc(0);
    return 0;
}
