// =============================
// File: fixed_proj.h
// =============================
#ifndef FIXED_PROJ_H
#define FIXED_PROJ_H

#include <stdint.h>
#include <stddef.h>
#include "flex_cluster_arch.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_printf.h"
#include "flex_dma_pattern.h"

/* -----------------------------------------------------------
 * Problem dimensions (FP32)
 * ----------------------------------------------------------- */
#define MAT_N               (256u)
#define STRIP               (64u)     /* strip size along each dimension */
#define ELEM_BYTES          (4u)      /* sizeof(float) */

#define A_ROWS              (MAT_N)
#define A_COLS              (MAT_N)
#define B_ROWS              (MAT_N)
#define B_COLS              (MAT_N)

#define A_STRIP_ROWS        (STRIP)         /* 64 */
#define A_STRIP_COLS        (MAT_N)         /* 256 */
#define B_STRIP_ROWS        (MAT_N)         /* 256 */
#define B_STRIP_COLS        (STRIP)         /* 64 */
#define C_TILE_ROWS         (STRIP)         /* 64 */
#define C_TILE_COLS         (STRIP)         /* 64 */

#define BYTES_A_STRIP       (A_STRIP_ROWS * A_STRIP_COLS * ELEM_BYTES)  /* 64*256*4 = 65536 */
#define BYTES_B_STRIP       (B_STRIP_ROWS * B_STRIP_COLS * ELEM_BYTES)  /* 256*64*4 = 65536 */
#define BYTES_C_TILE        (C_TILE_ROWS   * C_TILE_COLS   * ELEM_BYTES)/* 64*64*4  = 16384 */

/* -----------------------------------------------------------
 * HBM layout (offsets below are relative to ARCH_HBM_START_BASE)
 * Keep large gaps to avoid overlap.
 * ----------------------------------------------------------- */
#define HBM_A_BASE_OFFSET   (0x00000000u)
#define HBM_B_BASE_OFFSET   (0x01000000u)
#define HBM_C_BASE_OFFSET   (0x02000000u)

/* Compute HBM byte offset for A’s horizontal strip r (0..3). */
static inline uint32_t hbm_off_A_strip(uint32_t r)
{
    /* Row-major A: strip r starts at row r*64, contiguous 64*256 floats */
    const uint32_t rows_before = r * A_STRIP_ROWS;          /* r*64 */
    const uint32_t elems_before = rows_before * A_COLS;     /* r*64*256 */
    return HBM_A_BASE_OFFSET + (elems_before * ELEM_BYTES);
}

/* Compute parameters for B’s vertical strip c (0..3).
 * The strip starts at column c*64. For DMA-2D we use:
 *   size_per_row = 64*4 bytes,
 *   src_stride   = 256*4 bytes,
 *   repeat       = 256 rows.
 * Only the base byte offset is returned here. */
static inline uint32_t hbm_off_B_strip_base(uint32_t c)
{
    const uint32_t cols_before = c * B_STRIP_COLS;          /* c*64 */
    return HBM_B_BASE_OFFSET + (cols_before * ELEM_BYTES);
}

/* Compute HBM byte offset for C tile at (row=r, col=c) in the 4x4 grid. */
static inline uint32_t hbm_off_C_tile(uint32_t r, uint32_t c)
{
    const uint32_t row0 = r * C_TILE_ROWS;                  /* r*64 */
    const uint32_t col0 = c * C_TILE_COLS;                  /* c*64 */
    const uint32_t elem_index = row0 * MAT_N + col0;        /* (r*64)*256 + (c*64) */
    return HBM_C_BASE_OFFSET + (elem_index * ELEM_BYTES);
}

/* -----------------------------------------------------------
 * Broadcast masks for 4x4 clusters
 *   - For row broadcast (horizontal):  row_mask = 1<<row, col_mask = 0xF
 *   - For col broadcast (vertical):    row_mask = 0xF,    col_mask = 1<<col
 * ----------------------------------------------------------- */
static inline uint16_t mask_row(uint32_t row) { return (uint16_t)(1u << row); }
static inline uint16_t mask_col(uint32_t col) { return (uint16_t)(1u << col); }
static inline uint16_t mask_all4(void)        { return (uint16_t)0x000Fu; }

/* -----------------------------------------------------------
 * Small helpers for local TCDM pointer <-> offset conversions
 * ----------------------------------------------------------- */
static inline uint32_t tcdm_offset_from_ptr(uint32_t tcdm_base, void *ptr)
{
    return ((uint32_t)(uintptr_t)ptr) - tcdm_base;
}

#endif /* FIXED_PROJ_H */
