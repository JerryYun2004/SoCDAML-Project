#ifndef FIXED_PROJ_H
#define FIXED_PROJ_H

#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"

/* ===========================================================
 * Problem sizing (override in main before including if needed)
 * =========================================================== */
#ifndef MAT_N
#define MAT_N               (256u)
#endif

#ifndef TILE
#define TILE                (64u)
#endif

#ifndef ELEM_BYTES
#define ELEM_BYTES          (4u)    /* sizeof(float) */
#endif

/* Derived sizes */
#ifndef A_STRIP_ROWS
#define A_STRIP_ROWS        (TILE)      /* 64 */
#endif
#ifndef A_STRIP_COLS
#define A_STRIP_COLS        (MAT_N)     /* 256 */
#endif
#ifndef B_STRIP_ROWS
#define B_STRIP_ROWS        (MAT_N)     /* 256 */
#endif
#ifndef B_STRIP_COLS
#define B_STRIP_COLS        (TILE)      /* 64 */
#endif
#ifndef C_TILE_ROWS
#define C_TILE_ROWS         (TILE)      /* 64 */
#endif
#ifndef C_TILE_COLS
#define C_TILE_COLS         (TILE)      /* 64 */
#endif

#ifndef BYTES_A_STRIP
#define BYTES_A_STRIP       (A_STRIP_ROWS * A_STRIP_COLS * ELEM_BYTES) /* 64*256*4 = 65536 */
#endif
#ifndef BYTES_B_STRIP
#define BYTES_B_STRIP       (B_STRIP_ROWS * B_STRIP_COLS * ELEM_BYTES) /* 256*64*4 = 65536 */
#endif
#ifndef BYTES_C_TILE
#define BYTES_C_TILE        (C_TILE_ROWS   * C_TILE_COLS   * ELEM_BYTES) /* 64*64*4 = 16384 */
#endif

/* ===========================================================
 * HBM base offsets — inside your heap
 * Heap banner shows start at 0xC0000400 ⇒ offset 0x00000400.
 * Place A at heap start; B after A; C after B.
 * =========================================================== */
#ifndef HBM_A_BASE_OFFSET
#define HBM_A_BASE_OFFSET   (0x00000400u)
#endif
#ifndef HBM_B_BASE_OFFSET
#define HBM_B_BASE_OFFSET   (HBM_A_BASE_OFFSET + (MAT_N*MAT_N*ELEM_BYTES))
#endif
#ifndef HBM_C_BASE_OFFSET
#define HBM_C_BASE_OFFSET   (HBM_B_BASE_OFFSET + (MAT_N*MAT_N*ELEM_BYTES))
#endif

/* Row-major offsets into big A/B/C stored in HBM */
static inline uint32_t hbm_off_A_strip(uint32_t r)
{
    uint32_t rows_before  = r * A_STRIP_ROWS;     /* r*TILE */
    uint32_t elems_before = rows_before * MAT_N;  /* r*TILE*N */
    return HBM_A_BASE_OFFSET + elems_before * ELEM_BYTES;
}

/* B vertical strip base addr: column-block starting at c*TILE (row-major) */
static inline uint32_t hbm_off_B_strip_base(uint32_t c)
{
    uint32_t cols_before = c * B_STRIP_COLS;      /* c*TILE */
    return HBM_B_BASE_OFFSET + cols_before * ELEM_BYTES;
}

/* C tile (r,c) top-left */
static inline uint32_t hbm_off_C_tile(uint32_t r, uint32_t c)
{
    uint32_t row0       = r * C_TILE_ROWS;        /* r*TILE */
    uint32_t col0       = c * C_TILE_COLS;        /* c*TILE */
    uint32_t elem_index = row0 * MAT_N + col0;    /* row-major */
    return HBM_C_BASE_OFFSET + elem_index * ELEM_BYTES;
}

/* ===========================================================
 * Broadcast mask helpers (4x4 default)
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
 * TCDM pointer <-> offset helper
 * =========================================================== */
static inline uint32_t tcdm_offset_from_ptr(uint32_t tcdm_base, const void *ptr)
{
    return ((uint32_t)(uintptr_t)ptr) - tcdm_base;
}

/* ===========================================================
 * Small utils
 * =========================================================== */
static inline uint32_t align_up_u32(uint32_t v, uint32_t a /* power of 2 */)
{
    return (v + (a - 1u)) & ~(a - 1u);
}

static inline void *align_up_ptr(void *p, uint32_t a /* power of 2 */)
{
    uint32_t v  = (uint32_t)(uintptr_t)p;
    uint32_t va = align_up_u32(v, a);
    return (void *)(uintptr_t)va;
}

static inline void zero_f32(void *dst, uint32_t n_bytes)
{
    uint32_t *p = (uint32_t *)dst;
    uint32_t n  = n_bytes >> 2; /* words */
    for (uint32_t i = 0; i < n; ++i) { p[i] = 0u; }
}

static inline uint32_t checksum_u32(const void *ptr, uint32_t n_bytes)
{
    const uint32_t *p = (const uint32_t*)ptr;
    uint32_t n = n_bytes >> 2, s = 0u;
    for (uint32_t i = 0; i < n; ++i) { s ^= p[i]; }
    return s;
}

#endif /* FIXED_PROJ_H */
