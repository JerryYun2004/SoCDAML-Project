#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* ------------------- small helpers (no std headers) ------------------- */
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
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void*)v;
}

/* 64x256 * 256x64 -> 64x64 FP32 */
static void matmul_64x256_256x64(const float *A, const float *B, float *C)
{
    for (uint32_t i = 0; i < C_TILE_ROWS; ++i) {
        const float *a_row = &A[i * A_STRIP_COLS];
        float *c_row       = &C[i * C_TILE_COLS];
        for (uint32_t j = 0; j < C_TILE_COLS; ++j) {
            float acc = 0.0f;
            for (uint32_t k = 0; k < A_STRIP_COLS; ++k) {
                acc += a_row[k] * B[k * B_STRIP_COLS + j];
            }
            c_row[j] = acc;
        }
    }
}

/* one 1KB row write to HBM via DMA */
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* ------------------- one-time HBM init (A ramp, B identity w/2.0) ------------------- */
static void init_hbm_AB_fair_via_dma(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * ELEM_BYTES; /* 256*4 = 1024 */

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) {
        flex_eoc(1); return;
    }
    void *row_aln = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A: ramp (idx+1)/65536 */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            uint32_t idx = r * MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* B: identity with 2.0 on diag cols 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) pf[c] = 0.0f;
        if (r < MAT_N) {
            if ((r % C_TILE_COLS) == 0u) pf[r] = 2.0f;
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

    const uint32_t is_timer_master = (uint32_t)((P.x == 0u) & (P.y == 0u));
    if (is_timer_master) { flex_timer_start(); } 
    
    /* -------- L1 alloc (DM core per cluster) -------- */
    void *addr_a = (void*)0, *addr_b = (void*)0, *addr_c = (void*)0;
    uint32_t a_off = 0u, b_off = 0u, c_off = 0u;

    if (IS_DM) {
        void *raw_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        void *raw_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
        void *raw_c = flex_l1_malloc(BYTES_C_TILE  + 64u);
        if (!raw_a || !raw_b || !raw_c) {
            flex_eoc(1); return 1;
        }
        addr_a = align64(raw_a);
        addr_b = align64(raw_b);
        addr_c = align64(raw_c);

        a_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_a);
        b_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_b);
        c_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, addr_c);

        zero_f32(addr_c, BYTES_C_TILE);
    }

    if (is_timer_master) { flex_timer_end(); } 
    flex_global_barrier_xy();

    /* one-time HBM init by C[0,0] */
    init_hbm_AB_fair_via_dma();
    flex_global_barrier_xy();

    
    if (is_timer_master) { flex_timer_start(); } 
    /* -------- NONEXT LOADS (parallel DMA, no prints here) -------- */
    uint32_t info_offA = 0u, info_offB = 0u;
    uint32_t info_rows = (uint32_t)B_STRIP_ROWS;
    uint32_t a_hi = 0u, a_lo = 0u, b_hi = 0u, b_lo = 0u;
    
    if (IS_DM) {
        /* A: contiguous strip for row P.y (1D) */
        const uint32_t offA = hbm_off_A_strip(P.y);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
        bare_dma_wait_all();
        uint64_t sa = addsum_u32(addr_a, BYTES_A_STRIP);
        a_hi = hi32(sa); a_lo = lo32(sa);
        info_offA = offA;

        /* B: gather 256 rows of 64 (2D) for column P.x */
        const uint32_t offB_base    = hbm_off_B_strip_base(P.x);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;
        const uint32_t src_stride   = (uint32_t)MAT_N * ELEM_BYTES;        /* 256*4 */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                          size_per_row, dst_stride, src_stride, info_rows);
        bare_dma_wait_all();
        uint64_t sb = addsum_u32(addr_b, BYTES_B_STRIP);
        b_hi = hi32(sb); b_lo = lo32(sb);
        info_offB = offB_base;
    }

    if (is_timer_master) { flex_timer_end(); } 
    
    flex_global_barrier_xy();

    flex_eoc(0);
    return 0;
}
