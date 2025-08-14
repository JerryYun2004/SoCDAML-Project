// =============================
// File: fixed_proj.h (helpers + fallback sizing)
// =============================
#ifndef FIXED_PROJ_H
#define FIXED_PROJ_H

#include <stdint.h>
#include "flex_printf.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_dma_pattern.h"

/* Keep this header tiny and reusable. All problem sizing can be
 * overridden from main_test.c *before* including this header. */

/* ===========================================================
 * Problem sizing (fallback defaults; override if needed)
 * =========================================================== */
#ifndef MAT_N
#define MAT_N               (256u)
#endif
#ifndef TILE
#define TILE                (64u)
#endif
#ifndef ELEM_BYTES
#define ELEM_BYTES          (4u)      /* sizeof(float) */
#endif

/* Derived sizes */
#ifndef A_STRIP_ROWS
#define A_STRIP_ROWS        (TILE)    /* e.g., 64 */
#endif
#ifndef A_STRIP_COLS
#define A_STRIP_COLS        (MAT_N)   /* e.g., 256 */
#endif
#ifndef B_STRIP_ROWS
#define B_STRIP_ROWS        (MAT_N)   /* e.g., 256 */
#endif
#ifndef B_STRIP_COLS
#define B_STRIP_COLS        (TILE)    /* e.g., 64 */
#endif
#ifndef C_TILE_ROWS
#define C_TILE_ROWS         (TILE)    /* e.g., 64 */
#endif
#ifndef C_TILE_COLS
#define C_TILE_COLS         (TILE)    /* e.g., 64 */
#endif

#ifndef BYTES_A_STRIP
#define BYTES_A_STRIP       (A_STRIP_ROWS * A_STRIP_COLS * ELEM_BYTES)
#endif
#ifndef BYTES_B_STRIP
#define BYTES_B_STRIP       (B_STRIP_ROWS * B_STRIP_COLS * ELEM_BYTES)
#endif
#ifndef BYTES_C_TILE
#define BYTES_C_TILE        (C_TILE_ROWS   * C_TILE_COLS   * ELEM_BYTES)
#endif

/* ===========================================================
 * HBM base offsets (fallback defaults; override freely)
 * =========================================================== */
#ifndef HBM_A_BASE_OFFSET
#define HBM_A_BASE_OFFSET   (0x00000000u)
#endif
#ifndef HBM_B_BASE_OFFSET
#define HBM_B_BASE_OFFSET   (0x01000000u)
#endif
#ifndef HBM_C_BASE_OFFSET
#define HBM_C_BASE_OFFSET   (0x02000000u)
#endif

/* Row-major offsets:
 *  - A horizontal strip r: contiguous block of (TILE x N) floats
 *  - B vertical strip c:   first column = c*TILE (gather via 2D)
 *  - C tile (r,c):         top-left at (r*TILE, c*TILE)
 */
static inline uint32_t hbm_off_A_strip(uint32_t r)
{
    const uint32_t rows_before  = r * A_STRIP_ROWS;      /* r*TILE */
    const uint32_t elems_before = rows_before * MAT_N;   /* r*TILE*N */
    return HBM_A_BASE_OFFSET + elems_before * ELEM_BYTES;
}

static inline uint32_t hbm_off_B_strip_base(uint32_t c)
{
    const uint32_t cols_before = c * B_STRIP_COLS;       /* c*TILE */
    return HBM_B_BASE_OFFSET + cols_before * ELEM_BYTES;
}

static inline uint32_t hbm_off_C_tile(uint32_t r, uint32_t c)
{
    const uint32_t row0       = r * C_TILE_ROWS;         /* r*TILE */
    const uint32_t col0       = c * C_TILE_COLS;         /* c*TILE */
    const uint32_t elem_index = row0 * MAT_N + col0;     /* row-major */
    return HBM_C_BASE_OFFSET + elem_index * ELEM_BYTES;
}

/* ===========================================================
 * Broadcast mask helpers (assume 4x4 by default)
 * =========================================================== */
#ifndef FLEX_BCAST_NUM_ROWS
#define FLEX_BCAST_NUM_ROWS 4u
#endif
#ifndef FLEX_BCAST_NUM_COLS
#define FLEX_BCAST_NUM_COLS 4u
#endif
static inline uint16_t mask_row(uint32_t row_idx) { return (uint16_t)(1u << (row_idx & 0xF)); }
static inline uint16_t mask_col(uint32_t col_idx) { return (uint16_t)(1u << (col_idx & 0xF)); }
static inline uint16_t mask_all4(void)            { return (uint16_t)0x000Fu; }
static inline uint16_t mask_all_rows(void)        { return (uint16_t)((1u << FLEX_BCAST_NUM_ROWS) - 1u); }
static inline uint16_t mask_all_cols(void)        { return (uint16_t)((1u << FLEX_BCAST_NUM_COLS) - 1u); }

/* ===========================================================
 * L1 TCDM pointer <-> offset (for local()/remote_*())
 * =========================================================== */
static inline uint32_t tcdm_offset_from_ptr(uint32_t tcdm_base, const void *ptr)
{
    return ((uint32_t)(uintptr_t)ptr) - tcdm_base;
}

/* ===========================================================
 * Small utils
 * =========================================================== */
static inline void zero_f32(void *dst, uint32_t n_bytes)
{
    uint32_t *p = (uint32_t *)dst;  /* treat as words */
    uint32_t n  = n_bytes >> 2;     /* bytes -> words */
    for (uint32_t i = 0; i < n; ++i) p[i] = 0u;
}

static inline uint32_t checksum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t*)ptr;
    uint32_t n = n_bytes >> 2, s = 0u;
    for (uint32_t i = 0; i < n; ++i) s ^= p[i];
    return s;
}

static inline uint32_t ceil_div_u32(uint32_t a, uint32_t b)
{
    return (a + b - 1u) / b;
}

#endif /* FIXED_PROJ_H */
