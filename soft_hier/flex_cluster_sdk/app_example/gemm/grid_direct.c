/*
 * grid_direct.c -- cleaned benchmark_4.c from Jerry Yun's SoCDAML project.
 * Source revision: 707cc6f0db370d16f608de7b9ff86e9c758d95e9
 *
 * PURPOSE: Grid direct (slide 14): 16 workers independently fetch their A/B strips.
 * CURRENT SOURCE: A[256,256] * B[256,256] -> C[256,256], FP32, K=256.
 * Each active cluster computes a 64x64 output on its DM core using scalar C.
 * The slide workload has K=64 and describes Spatz; that historical kernel and
 * configuration have not been recovered. This file is the audited source
 * baseline, not a verified reproduction of the slide timings.
 *
 * Coordinates are C[y,x]. Matrices are row-major. The hardware is a 4x4 grid;
 * idle cores still join global barriers. Worker eligibility is explicit below.
 * HBM input initialization is outside the measured DMA/compute intervals.
 * Existing timer/barrier placement is preserved; interval names are comments
 * only. A master-local compute interval is not the slowest worker's runtime.
 * See gemm/README.md and docs/project/REPRODUCTION.md for boundaries and limits.
 *
 * Attribution: original benchmark by Jerry Yun, using the ETH Zurich / PULP
 * SoftHier SDK and GVSoC. Existing dependency authorship/licenses still apply.
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* fixed_proj.h defines MAT_N=256, TILE=64 and FP32 HBM strides.
 * A worker at C[y,x] needs A rows 64*y..64*y+63 and
 * B columns 64*x..64*x+63; it owns that 64x64 C output tile. */

/* DMA buffers use 64-byte alignment. Retain the raw allocation pointer for free(). */
static inline void *align64(void *p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v + 63u) & ~(uintptr_t)63u;
    return (void*)v;
}

/* Original scalar FP32 GEMM loop. Only a worker DM core calls it; no Spatz dispatch occurs. */
static void gemm_scalar_fp32(const float *A, const float *B, float *C)
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

/* Blocking row write used only to initialize the HBM test input. */
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(hbm_addr(hbm_off_row), local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* Untimed HBM initialization by C[0,0]: A is a ramp; B is diagonal (1 or 2).
 * Allocate this scratch row after worker buffers so broadcast destination offsets agree. */
static void init_hbm_AB_fair_via_dma(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * ELEM_BYTES;

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) {
        printf("[InitHBM] ERROR: L1 row alloc failed\n");
        flex_eoc(1); return;
    }
    void *row_aln = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    for (uint32_t r = 0; r < MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < MAT_N; ++c) {
            uint32_t idx = r * MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

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

    printf("[InitHBM] A: ramp; B: identity (2.0 on columns {0,64,128,192})\n");
}

/* Every core must follow the same global-barrier sequence, even when idle. */
int main(void)
{
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    flex_alloc_init();
    flex_global_barrier_xy();

    const uint32_t cid   = flex_get_cluster_id();
    const uint32_t core  = flex_get_core_id();
    const FlexPosition P = get_pos(cid);
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t is_timer_master = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u));

    /* Timer 1: allocation_and_zero. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    void *addr_a = (void*)0, *addr_b = (void*)0, *addr_c = (void*)0;
    uint32_t a_off = 0u, b_off = 0u, c_off = 0u;

    if (IS_DM) {
        void *raw_a = flex_l1_malloc(BYTES_A_STRIP + 64u);
        void *raw_b = flex_l1_malloc(BYTES_B_STRIP + 64u);
        void *raw_c = flex_l1_malloc(BYTES_C_TILE  + 64u);
        if (!raw_a || !raw_b || !raw_c) {
            printf("[ERR] L1 malloc failed: A=%p B=%p C=%p\n", raw_a, raw_b, raw_c);
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

    /* Timer 2: sync_after_allocation. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    init_hbm_AB_fair_via_dma();
    flex_global_barrier_xy();

    /* Timer 3: input_dma_with_barriers. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    /* Sweep rows in order; only the selected worker/leader issues DMA. */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();

            if (IS_DM && P.y == ry && P.x == rx) {
                const uint32_t offA = hbm_off_A_strip(ry);
                bare_dma_start_1d(local(a_off), hbm_addr(offA), BYTES_A_STRIP);
                bare_dma_wait_all();

                const uint32_t offB_base    = hbm_off_B_strip_base(rx);
                const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES;
                const uint32_t dst_stride   = size_per_row;
                const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;
                const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;

                /* 2D DMA arguments: destination, source, bytes per row,
                 * destination row stride, source row stride, row count.
                 * Gather packs a B column tile; scatter writes a C column tile. */
                bare_dma_start_2d(local(b_off), hbm_addr(offB_base),
                                  size_per_row, dst_stride, src_stride, repeat);
                bare_dma_wait_all();
            }
            flex_global_barrier_xy();
        }
    }
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 4: sync_after_input. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 5: compute. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    if (IS_DM) {
        float *A = (float*)(uintptr_t)local(a_off);
        float *B = (float*)(uintptr_t)local(b_off);
        float *C = (float*)(uintptr_t)local(c_off);

        gemm_scalar_fp32((const float*)A, (const float*)B, (float*)C);
    }
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 6: sync_after_compute. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 7: output_dma_with_barriers. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    /* Sweep rows in order; only the selected worker/leader issues DMA. */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();

            if (IS_DM && P.y == ry && P.x == rx) {
                const uint32_t offC_this = hbm_off_C_tile(P.y, P.x);
                const uint32_t size_row  = (uint32_t)C_TILE_COLS * ELEM_BYTES;
                const uint32_t dst_str   = (uint32_t)MAT_N * ELEM_BYTES;
                const uint32_t src_str   = size_row;
                const uint32_t reps      = (uint32_t)C_TILE_ROWS;
                /* 2D DMA arguments: destination, source, bytes per row,
                 * destination row stride, source row stride, row count.
                 * Gather packs a B column tile; scatter writes a C column tile. */
                bare_dma_start_2d(hbm_addr(offC_this), local(c_off),
                                  size_row, dst_str, src_str, reps);
                bare_dma_wait_all();
            }

            flex_global_barrier_xy();
        }
    }
    if (is_timer_master) { flex_timer_end(); }

    /* Timer 8: final_sync. Master C[0,0] DM only. */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    if (cid == 0u && core == 0u) {
        printf("[Done][NONEXT] All tiles stored at base 0x%08x\n", (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
