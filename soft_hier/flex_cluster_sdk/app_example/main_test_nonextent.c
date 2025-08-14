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
        printf("[InitHBM] ERROR: L1 row buffer alloc failed\n");
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

    printf("[InitHBM] A: ramp; B: identity (2.0 on columns {0,64,128,192})\n");
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

    if (cid == 0u && core == 0u) {
        printf("[Info][NONEXT] 4x4 cluster GEMM (FP32) — each cluster fetches its own A/B from HBM\n");
        printf("       Grid=(%u x %u), cores/cluster=%u\n",
               (unsigned)ARCH_NUM_CLUSTER_X, (unsigned)ARCH_NUM_CLUSTER_Y,
               (unsigned)ARCH_NUM_CORE_PER_CLUSTER);
        printf("       N=%u, TILE=%u, elem_bytes=%u\n",
               (unsigned)MAT_N, (unsigned)C_TILE_ROWS, (unsigned)ELEM_BYTES);
        printf("       A-strip bytes=%u, B-strip bytes=%u, C-tile bytes=%u\n",
               (unsigned)BYTES_A_STRIP, (unsigned)BYTES_B_STRIP, (unsigned)BYTES_C_TILE);
    }
    flex_global_barrier_xy();

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
            printf("[ERR] L1 malloc failed: A=%p B=%p C=%p\n", raw_a, raw_b, raw_c);
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
    flex_global_barrier_xy();

    /* Ordered offsets print (row-major over clusters) */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                printf("[C%u,%u] L1 offsets (aligned): A=%u B=%u C=%u\n",
                       (unsigned)P.y, (unsigned)P.x,
                       (unsigned)a_off, (unsigned)b_off, (unsigned)c_off);
            }
        }
    }
    flex_global_barrier_xy();

    /* ===============================================================
     * 2) One-time HBM init via DMA (runs only on C[0,0] DM)
     * =============================================================== */
    init_hbm_AB_fair_via_dma();
    flex_global_barrier_xy();

    /* ===============================================================
     * 3) NON-EXTENDED LOADS: every cluster DM core fetches its own A/B stripes
     * =============================================================== */
    if (IS_DM) {
        /* A-strip for row P.y: contiguous 64x256 block (1D DMA) */
        const uint32_t offA = hbm_off_A_strip(P.y);
        bare_dma_start_1d(/*dst*/ local(a_off), /*src*/ hbm_addr(offA), BYTES_A_STRIP);
        bare_dma_wait_all();
        uint64_t sa = addsum_u32(addr_a, BYTES_A_STRIP);
        printf("[Load][A][NONEXT] C[%u,%u](DM) off=0x%08x bytes=%u | add=0x%08x%08x\n",
               (unsigned)P.y, (unsigned)P.x, (unsigned)offA, (unsigned)BYTES_A_STRIP,
               (unsigned)hi32(sa), (unsigned)lo32(sa));

        /* B-strip for col P.x: gather 256 rows of 64 elems (2D DMA) */
        const uint32_t offB_base    = hbm_off_B_strip_base(P.x);
        const uint32_t size_per_row = (uint32_t)B_STRIP_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_stride   = size_per_row;                        /* pack */
        const uint32_t src_stride   = (uint32_t)MAT_N * ELEM_BYTES;        /* 256*4 */
        const uint32_t repeat       = (uint32_t)B_STRIP_ROWS;              /* 256  */
        bare_dma_start_2d(/*dst*/ local(b_off), /*src*/ hbm_addr(offB_base),
                          size_per_row, dst_stride, src_stride, repeat);
        bare_dma_wait_all();
        uint64_t sb = addsum_u32(addr_b, BYTES_B_STRIP);
        printf("[Load][B][NONEXT] C[%u,%u](DM) base=0x%08x rows=%u | add=0x%08x%08x\n",
               (unsigned)P.y, (unsigned)P.x, (unsigned)offB_base, (unsigned)repeat,
               (unsigned)hi32(sb), (unsigned)lo32(sb));
    }
    flex_global_barrier_xy();

    /* ===============================================================
     * 4) PARALLEL compute on every DM core in all clusters
     * =============================================================== */
    uint32_t cs_hi = 0u, cs_lo = 0u;
    uint32_t offC  = 0u;

    if (IS_DM) {
        float *A = (float*)(uintptr_t)local(a_off);
        float *B = (float*)(uintptr_t)local(b_off);
        float *C = (float*)(uintptr_t)local(c_off);

        matmul_64x256_256x64((const float*)A, (const float*)B, (float*)C);

        uint64_t sc = addsum_u32(C, BYTES_C_TILE);
        cs_hi = hi32(sc);
        cs_lo = lo32(sc);
        offC  = hbm_off_C_tile(P.y, P.x);
    }
    flex_global_barrier_xy();

    /* ===============================================================
     * 5) PARALLEL store to HBM
     * =============================================================== */
    if (IS_DM) {
        const uint32_t size_row = (uint32_t)C_TILE_COLS * ELEM_BYTES; /* 64*4 */
        const uint32_t dst_str  = (uint32_t)MAT_N * ELEM_BYTES;       /* 256*4 */
        const uint32_t src_str  = size_row;                           /* packed */
        const uint32_t reps     = (uint32_t)C_TILE_ROWS;              /* 64    */
        bare_dma_start_2d(/*dst*/ hbm_addr(offC), /*src*/ local(c_off),
                          size_row, dst_str, src_str, reps);
        bare_dma_wait_all();
    }
    flex_global_barrier_xy();

    /* ===============================================================
     * 6) Ordered prints only (after parallel work is done)
     * =============================================================== */
    for (uint32_t ry = 0; ry < 4u; ++ry) {
        for (uint32_t rx = 0; rx < 4u; ++rx) {
            flex_global_barrier_xy();
            if (IS_DM && P.y == ry && P.x == rx) {
                printf("[Compute][PAR][NONEXT] C[%u,%u] add=0x%08x%08x\n",
                       (unsigned)ry, (unsigned)rx, (unsigned)cs_hi, (unsigned)cs_lo);
                printf("[Store][PAR][NONEXT]   C[%u,%u] -> HBM off=0x%08x, bytes/row=%u, reps=%u\n",
                       (unsigned)ry, (unsigned)rx, (unsigned)offC,
                       (unsigned)((uint32_t)C_TILE_COLS * ELEM_BYTES), (unsigned)C_TILE_ROWS);
            }
        }
    }
    flex_global_barrier_xy();

    if (cid == 0u && core == 0u) {
        printf("[Done][NONEXT] All 16 tiles of C written to HBM at base 0x%08x\n",
               (unsigned)HBM_C_BASE_OFFSET);
    }

    flex_eoc(0);
    return 0;
}
