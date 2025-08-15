#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* ---------- 64-bit additive checksum over 32-bit words ---------- */
static inline uint64_t addsum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t *)ptr;
    uint32_t n = n_bytes >> 2;
    uint64_t s = 0;
    for (uint32_t i = 0; i < n; ++i) s += (uint64_t)p[i];
    return s;
}
static inline uint32_t hi32(uint64_t v) { return (uint32_t)(v >> 32); }
static inline uint32_t lo32(uint64_t v) { return (uint32_t)(v & 0xFFFFFFFFu); }

/* ---------- GEMM: 64x256 * 256x64 -> 64x64 (FP32) ---------- */
static void matmul_64x256_256x64(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < C_TILE_ROWS; ++i) {
        float *c_row = &C[i * C_TILE_COLS];
        const float *a_row = &A[i * A_STRIP_COLS];
        for (uint32_t j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < A_STRIP_COLS; ++k) {
                acc += a_row[k] * B[k * B_STRIP_COLS + j];
            }
            c_row[j] = acc;
        }
    }
}

/* ---------- DMA helper: write one 256-element row (1024B) to HBM ---------- */
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t row_off_l1, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(row_off_l1), row_bytes);
    bare_dma_wait_all();
}

/* ---------- One-time HBM init via DMA (A ramp; B identity w/ 2.0 on cols {0,64,128,192}) ----------
 * Runs only on DM core of C[0,0]. Call AFTER L1 A/B/C are allocated so offsets match everywhere.
 */
static void init_hbm_AB_fair_via_dma(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);
    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ALIGN_DMA = 64u;
    const uint32_t ROW_BYTES = (uint32_t)MAT_N * ELEM_BYTES;  /* 256*4 = 1024 */

    /* 64B-aligned row buffer in L1 */
    void *row_raw = flex_l1_malloc(ROW_BYTES + ALIGN_DMA);
    if (row_raw == (void*)0) {
        flex_eoc(1);
        return;
    }
    void *row_aln = align_up_ptr(row_raw, ALIGN_DMA);
    uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* ---- Fill A (ramp) row by row and DMA to HBM ---- */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            uint32_t idx = r * MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* ---- Fill B (identity; diag=2.0 at cols 0/64/128/192; else diag=1.0) ---- */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) { pf[c] = 0.0f; }
        if (r < MAT_N) {
            if ((r % C_TILE_COLS) == 0u) pf[r] = 2.0f;
            else                          pf[r] = 1.0f;
        }
        const uint32_t offB_row = HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }
}

int main(void)
{
    /* ---- Bring-up ---- */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t IS_DM = flex_is_dm_core();

    /* convenience predicate: only C[0,0] DM emits timer stamps */
    const uint32_t is_timer_master = (uint32_t)((P.x == 0u) & (P.y == 0u));
    if (is_timer_master) { flex_timer_start(); } 
    /* ===============================================================
     * 1) L1 allocations (same order on all clusters) BEFORE HBM init
     * =============================================================== */
    void *addr_a = (void*)0, *addr_b = (void*)0, *addr_c = (void*)0;
    uint32_t a_off = 0u, b_off = 0u, c_off = 0u;

    if (IS_DM) {
        const uint32_t ALIGN_DMA = 64u;
        void *raw_a = flex_l1_malloc(BYTES_A_STRIP + ALIGN_DMA);
        void *raw_b = flex_l1_malloc(BYTES_B_STRIP + ALIGN_DMA);
        void *raw_c = flex_l1_malloc(BYTES_C_TILE  + ALIGN_DMA);
        if (!raw_a || !raw_b || !raw_c) {
            flex_eoc(1); return 1;
        }
        addr_a = align_up_ptr(raw_a, ALIGN_DMA);
        addr_b = align_up_ptr(raw_b, ALIGN_DMA);
        addr_c = align_up_ptr(raw_c, ALIGN_DMA);

        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);

        zero_f32(addr_c, BYTES_C_TILE);
    }

    if (is_timer_master) { flex_timer_end(); }
    flex_global_barrier_xy();

    /* ===============================================================
     * 2) One-time HBM init via DMA (runs only on C[0,0] DM)
     * =============================================================== */
    init_hbm_AB_fair_via_dma();
    flex_global_barrier_xy();

  
    /* ===============================================================
     * 3) Leaders load stripes from HBM (DM core only), ordered prints
     * =============================================================== */
    
  
    /* Row leaders (x==0) load A_r with 1D contiguous DMA */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        flex_global_barrier_xy();
        if (IS_DM && P.x == 0u && P.y == ry) {
            const uint32_t offA = hbm_off_A_strip(ry);
            bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
            bare_dma_wait_all();
            uint64_t sa = addsum_u32(addr_a, BYTES_A_STRIP);
        }
    }
    flex_global_barrier_xy();

    /* Column leaders (y==0) load B_c with 2D gather DMA */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (IS_DM && P.y == 0u && P.x == rx) {
            const uint32_t offB_base    = hbm_off_B_strip_base(rx);
            const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
            const uint32_t dst_stride   = size_per_row;                        /* pack */
            const uint32_t src_stride   = (uint32_t)MAT_N * ELEM_BYTES;        /* 256*4 */
            const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;              /* 256  */
            bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                              size_per_row, dst_stride, src_stride, repeat);
            bare_dma_wait_all();
            uint64_t sb = addsum_u32(addr_b, BYTES_B_STRIP);
        }
    }
    flex_global_barrier_xy();

    /* ===============================================================
     * 4) Inter-cluster broadcasts (DM core only)
     *    A horizontally by row; B vertically by column
     * =============================================================== */

    /* Broadcast A rows */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        flex_global_barrier_xy();
        if (IS_DM && P.x == 0u && P.y == ry) {
            const uint16_t row_m = mask_row(ry);
            const uint16_t col_m = mask_all4();
            flex_dma_async_broadcast(/*dst_off*/ a_off, /*src_off*/ a_off,
                                     BYTES_A_STRIP, row_m, col_m);
            flex_dma_async_wait_all();
        }
    }
    flex_global_barrier_xy();

    /* Broadcast B columns */
    for (uint32_t rx = 0; rx < 4u; ++rx) {
        flex_global_barrier_xy();
        if (IS_DM && P.y == 0u && P.x == rx) {
            const uint16_t row_m = mask_all4();
            const uint16_t col_m = mask_col(rx);
            flex_dma_async_broadcast(/*dst_off*/ b_off, /*src_off*/ b_off,
                                     BYTES_B_STRIP, row_m, col_m);
            flex_dma_async_wait_all();
        }
    }
    flex_global_barrier_xy();


    flex_eoc(0);
    return 0;
}
