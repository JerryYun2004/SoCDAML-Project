/* benchmark_4.c — 4x4 tiled GEMM (FP32), direct HBM loads (no intercluster broadcast)
 *
 * Full GEMM: A[256,256] * B[256,256] -> C[256,256]
 * Per-cluster tile (DM core):
 *   A_tile: 64 x 256   (row block)
 *   B_tile: 256 x 64   (col block)
 *   C_tile: 64 x 64
 *
 * Timers (C[0,0] only):
 *   DATA-IN  : serialized HBM→L1 loads for ALL 16 tiles (measured via HBM token ring)
 *   SYNC(1)  : barrier after loads
 *   COMPUTE  : per-cluster matmul
 *   SYNC(2)  : barrier after compute
 *   DATA-OUT : L1→HBM stores (parallel)
 *   SYNC(3)  : final barrier
 */

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"
#include "fixed_proj.h"

/* ------------------- small helpers ------------------- */
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

/* 64x256 * 256x64 -> 64x64 FP32 (per-tile compute) */
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

/* one 1KB row write to HBM via DMA (used by preload) */
static inline void dma_write_row(uint32_t hbm_off_row, uint32_t l1_off_row, uint32_t row_bytes)
{
    bare_dma_start_1d(/*dst*/ hbm_addr(hbm_off_row), /*src*/ local(l1_off_row), row_bytes);
    bare_dma_wait_all();
}

/* ------------------- one-time HBM init (A ramp, B identity w/2.0) — NOT TIMED ------------------- */
static void init_hbm_AB_fair_via_dma(void)
{
    const uint32_t IS_DM = flex_is_dm_core();
    const uint32_t cid   = flex_get_cluster_id();
    const FlexPosition P = get_pos(cid);

    if (!(IS_DM && P.x == 0u && P.y == 0u)) return;

    const uint32_t ROW_BYTES = (uint32_t)MAT_N * ELEM_BYTES; /* 256*4 = 1024 */

    void *row_raw = flex_l1_malloc(ROW_BYTES + 64u);
    if (row_raw == (void*)0) {
        printf("[InitHBM] ERROR: L1 row alloc failed\n");
        flex_eoc(1); return;
    }
    void *row_aln = align64(row_raw);
    const uint32_t row_off = tcdm_offset_from_ptr((uint32_t)ARCH_CLUSTER_TCDM_BASE, row_aln);

    /* A: ramp (idx+1)/65536 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) {
            uint32_t idx = r * (uint32_t)MAT_N + c;
            pf[c] = (float)(idx + 1u) * (1.0f / 65536.0f);
        }
        const uint32_t offA_row = (uint32_t)HBM_A_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offA_row, row_off, ROW_BYTES);
    }

    /* B: identity with 2.0 on diag cols 0,64,128,192; else diag=1.0 */
    for (uint32_t r = 0; r < (uint32_t)MAT_N; ++r) {
        float *pf = (float*)(uintptr_t)local(row_off);
        for (uint32_t c = 0; c < (uint32_t)MAT_N; ++c) pf[c] = 0.0f;
        if ((r % (uint32_t)C_TILE_COLS) == 0u) pf[r] = 2.0f;
        else                                    pf[r] = 1.0f;
        const uint32_t offB_row = (uint32_t)HBM_B_BASE_OFFSET + r * ROW_BYTES;
        dma_write_row(offB_row, row_off, ROW_BYTES);
    }

    printf("[InitHBM] A: ramp; B: identity (2.0 on columns {0,64,128,192})\n");
}

/* ------------------- global token (HBM) for serialized DMA timing -------------------
 * We place it just after the full C buffer in HBM to avoid overlap.
 */
#ifndef ELEM_BYTES
#define ELEM_BYTES 4u
#endif
#define C_FULL_BYTES   ((uint32_t)MAT_N * (uint32_t)MAT_N * (uint32_t)ELEM_BYTES)
#define TOKEN_OFFSET   ((uint32_t)HBM_C_BASE_OFFSET + (uint32_t)C_FULL_BYTES + 0x100u)

static inline volatile uint32_t* hbm_token_ptr(void)
{
    return (volatile uint32_t*)(uintptr_t)hbm_addr((uint32_t)TOKEN_OFFSET);
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
    const uint32_t is_timer_master = (uint32_t)(IS_DM && (P.x == 0u) && (P.y == 0u)); /* C[0,0] DM prints timers */

    /* -------- L1 alloc (DM core per cluster) -------- */
    void *addr_a = (void*)0, *addr_b = (void*)0, *addr_c = (void*)0;
    uint32_t a_off = 0u, b_off = 0u, c_off = 0u;
    if (is_timer_master) { flex_timer_start(); }
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

    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* one-time HBM init by C[0,0] — NOT TIMED */
    init_hbm_AB_fair_via_dma();
    flex_global_barrier_xy();

    /* ====================== DATA-IN (serialized over all 16 tiles) ======================
     * Implemented via a global HBM token; C00 measures the whole sweep (0..16).
     */
    volatile uint32_t * const token = hbm_token_ptr();

    /* Initialize token and synchronize before starting the sweep */
    if (is_timer_master) { *token = 0u; }
    flex_global_barrier_xy();   /* make sure everyone sees token=0 */

    const uint32_t my_tile = (uint32_t)(P.y * 4u + P.x);  /* 0..15 for 4x4 grid */

    if (is_timer_master) { flex_timer_start(); }  /* DATA-IN (collective) start */

    for (uint32_t t = 0; t < 16u; ++t) {
        /* All DM cores spin until it's time for tile t (C00 spins too, so its timer accrues). */
        if (IS_DM) {
            while (*token != t) { /* busy-wait: keep the core active */ }
            if (my_tile == t) {
                /* ---- This tile's DM core pulls its A strip and B strip ---- */

                /* A: contiguous 1D strip for row = P.y */
                const uint32_t offA = hbm_off_A_strip(P.y);
                bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
                bare_dma_wait_all();

                /* B: 2D gather for column = P.x */
                const uint32_t offB_base    = hbm_off_B_strip_base(P.x);
                const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * (uint32_t)ELEM_BYTES; /* 64*4 */
                const uint32_t dst_stride   = size_per_row;
                const uint32_t src_stride   = (uint32_t)MAT_N * (uint32_t)ELEM_BYTES;        /* 256*4 */
                const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;                         /* 256   */

                bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                                  size_per_row, dst_stride, src_stride, repeat);
                bare_dma_wait_all();

                /* Publish completion of tile t */
                *token = t + 1u;
            }
        }
        /* Non-DM cores do nothing here; they'll join at the next global barrier. */
    }

    if (is_timer_master) { flex_timer_end(); }    /* DATA-IN (collective) end */

    /* Ensure everyone observes token == 16 (end of loads) and rejoins */
    flex_global_barrier_xy();

    /* ====================== SYNC(1): after loads ====================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* ====================== COMPUTE (tile) ====================== */
    if (is_timer_master) { flex_timer_start(); }      /* COMPUTE start */

    if (IS_DM) {
        float *A = (float*)(uintptr_t)local(a_off);
        float *B = (float*)(uintptr_t)local(b_off);
        float *C = (float*)(uintptr_t)local(c_off);

        /* matmul per tile */
        matmul_64x256_256x64((const float*)A, (const float*)B, (float*)C);

        /* (Optional) checksum if you want to keep it; outside of timers normally: */
        // uint64_t sc = addsum_u32(C, BYTES_C_TILE);
        // (void)sc;
    }

    if (is_timer_master) { flex_timer_end(); }        /* COMPUTE end */

    /* ====================== SYNC(2): after compute ====================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    /* ====================== DATA-OUT (L1 -> HBM, parallel) ====================== */
    if (is_timer_master) { flex_timer_start(); }      /* DATA-OUT start */

    if (IS_DM) {
        const uint32_t offC     = hbm_off_C_tile(P.y, P.x);
        const uint32_t size_row = (uint32_t)C_TILE_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_str  = (uint32_t)MAT_N * ELEM_BYTES;       /* 256*4 */
        const uint32_t src_str  = size_row;
        const uint32_t reps     = (uint32_t)C_TILE_ROWS;              /* 64    */
        bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                          size_row, dst_str, src_str, reps);
        bare_dma_wait_all();
    }

    if (is_timer_master) { flex_timer_end(); }        /* DATA-OUT end */

    /* ====================== SYNC(3): final ====================== */
    if (is_timer_master) { flex_timer_start(); }
    flex_global_barrier_xy();
    if (is_timer_master) { flex_timer_end(); }

    if (cid == 0u && core == 0u) {
        printf("[Done][NONEXT] All tiles stored at base 0x%08x\n", (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
