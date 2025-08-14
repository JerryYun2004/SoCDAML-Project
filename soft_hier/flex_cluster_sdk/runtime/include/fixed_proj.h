#ifndef FIXED_PROJ_H
#define FIXED_PROJ_H

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "flex_runtime.h"       // for FlexPosition, local(), hbm_addr(), barriers, ARCH_* macros
#include "flex_alloc.h"         // for flex_l1_malloc, flex_alloc_init
#include "flex_dma_pattern.h"   // for DMA/broadcast functions

/* -----------------------------------------------------------
 * Problem dimensions and element size (can be changed for other sizes)
 * ----------------------------------------------------------- */
#define MAT_N               (256u)
#define TILE                (64u)
#define ELEM_BYTES          (4u)    /* sizeof(float) */

#define A_STRIP_ROWS        (TILE)
#define A_STRIP_COLS        (MAT_N)
#define B_STRIP_ROWS        (MAT_N)
#define B_STRIP_COLS        (TILE)
#define C_TILE_ROWS         (TILE)
#define C_TILE_COLS         (TILE)

#define BYTES_A_STRIP       (A_STRIP_ROWS * A_STRIP_COLS * ELEM_BYTES)
#define BYTES_B_STRIP       (B_STRIP_ROWS * B_STRIP_COLS * ELEM_BYTES)
#define BYTES_C_TILE        (C_TILE_ROWS   * C_TILE_COLS   * ELEM_BYTES)

/* -----------------------------------------------------------
 * HBM base offsets (adjust as needed)
 * ----------------------------------------------------------- */
#define HBM_A_BASE_OFFSET   (0x00000000u)
#define HBM_B_BASE_OFFSET   (0x01000000u)
#define HBM_C_BASE_OFFSET   (0x02000000u)

/* -----------------------------------------------------------
 * Offset computation helpers (row-major layout)
 * ----------------------------------------------------------- */
static inline uint32_t hbm_off_A_strip(uint32_t r) {
    return HBM_A_BASE_OFFSET + (r * A_STRIP_ROWS * MAT_N * ELEM_BYTES);
}

static inline uint32_t hbm_off_B_strip_base(uint32_t c) {
    return HBM_B_BASE_OFFSET + (c * B_STRIP_COLS * ELEM_BYTES);
}

static inline uint32_t hbm_off_C_tile(uint32_t r, uint32_t c) {
    return HBM_C_BASE_OFFSET + ((r * C_TILE_ROWS * MAT_N + c * C_TILE_COLS) * ELEM_BYTES);
}

/* -----------------------------------------------------------
 * Broadcast mask helpers for 4x4 clusters
 * ----------------------------------------------------------- */
static inline uint16_t mask_row(uint32_t row) { return (uint16_t)(1u << row); }
static inline uint16_t mask_col(uint32_t col) { return (uint16_t)(1u << col); }
static inline uint16_t mask_all4(void)        { return (uint16_t)0x000Fu; }

/* -----------------------------------------------------------
 * L1 TCDM pointer <-> offset conversion helpers
 * ----------------------------------------------------------- */
static inline uint32_t tcdm_offset_from_ptr(uint32_t tcdm_base, void *ptr) {
    return ((uint32_t)(uintptr_t)ptr) - tcdm_base;
}

/* -----------------------------------------------------------
 * Misc helpers
 * ----------------------------------------------------------- */
static inline uint32_t ceil_div_u32(uint32_t a, uint32_t b) {
    return (a + b - 1u) / b;
}

static inline void zero_f32(void *dst, uint32_t n_bytes) {
    uint32_t *p = (uint32_t *)dst;
    uint32_t n  = n_bytes >> 2;
    for (uint32_t i = 0; i < n; ++i) p[i] = 0u;
}

static inline uint32_t checksum_u32(const void *ptr, uint32_t n_bytes) {
    const uint32_t *p = (const uint32_t*)ptr;
    uint32_t n = n_bytes >> 2, s = 0u;
    for (uint32_t i = 0; i < n; ++i) s ^= p[i];
    return s;
}

#endif /* FIXED_PROJ_H */
