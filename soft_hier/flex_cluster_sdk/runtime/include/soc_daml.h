#pragma once
/*
 * soc_daml.h — HBM-visible allocator metadata & helpers (bitmap "in-between" rule)
 *
 * Additions over the strict match version:
 *  - Partition L1 into fixed-size slots (S = 0x400).
 *  - For each (cluster,core) snapshot, build a FREE bitmap over slots.
 *  - Cluster-common bitmap = AND of core bitmaps in that cluster.
 *  - System-common  bitmap = AND of cluster-common bitmaps across clusters.
 *  - Convert common bitmaps back to contiguous (addr,size) ranges for printing.
 *
 * Strict tuples (addr,size) data are still captured by the snapshot, but the
 * intersections run on bitmaps instead of exact tuple equality.
 */

#include <stdint.h>
#include <stddef.h>
#include "flex_cluster_arch.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_printf.h"

/* --------------------------
 * Dimensions from platform
 * -------------------------- */
#if defined(ARCH_NUM_CLUSTER)
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER)
#else
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y)
#endif
#define CORES_PER_CLUSTER    (ARCH_NUM_CORE_PER_CLUSTER)

/* Heap bounds (per cluster, same numeric range in each cluster’s L1) */
#define HEAP_BASE   (ARCH_CLUSTER_HEAP_BASE)
#define HEAP_END    (ARCH_CLUSTER_HEAP_END)
#define HEAP_SIZE   ((uint32_t)((HEAP_END) - (HEAP_BASE)))

/* Slot size (granularity for the in-between rule) */
#define SLOT_SIZE   (0x400u)  /* 1 KiB */

/* Derived slot counts/bitmap width */
#define NUM_SLOTS       ((uint32_t)(((HEAP_SIZE) + (SLOT_SIZE) - 1u) / (SLOT_SIZE)))
#define BITMAP_WORDS    ((uint32_t)(((NUM_SLOTS) + 31u) / 32u))

/* Tunables for snapshot list → we still capture the free list for building bitmaps */
#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE  128u
#endif
#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS         128u   /* limits for converted ranges we emit for printing */
#endif

/* --------------------------
 * Types
 * -------------------------- */
typedef struct {
  void    *addr;   /* address of free block header in L1 */
  uint32_t size;   /* size in bytes */
} daml_block_t;

/* --------------------------
 * HBM-resident global state
 * -------------------------- */
/* Per-(cluster,core) allocators (their free lists live in L1) */
__attribute__((section(".hbm")))
alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

/* Per-(cluster,core) snapshots of the free list */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_count[NUM_CLUSTERS][CORES_PER_CLUSTER];
__attribute__((section(".hbm")))
daml_block_t g_hbm_core_free[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

/* NEW: per-(cluster,core) FREE bitmap (over slots) */
__attribute__((section(".hbm")))
uint32_t g_hbm_core_free_bitmap[NUM_CLUSTERS][CORES_PER_CLUSTER][BITMAP_WORDS];

/* Cluster-common and system-common bitmaps */
__attribute__((section(".hbm")))
uint32_t g_hbm_cluster_common_bitmap[NUM_CLUSTERS][BITMAP_WORDS];
__attribute__((section(".hbm")))
uint32_t g_hbm_system_common_bitmap[BITMAP_WORDS];

/* Converted ranges from the common bitmaps (for printing / easy inspection) */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_cluster_common_count[NUM_CLUSTERS];
__attribute__((section(".hbm")))
daml_block_t g_hbm_cluster_common[NUM_CLUSTERS][MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_count;
__attribute__((section(".hbm")))
daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

/* Simple locks in HBM (available if you need them later) */
__attribute__((section(".hbm")))
volatile int g_cluster_lock[NUM_CLUSTERS];
__attribute__((section(".hbm")))
volatile int g_global_lock;

/* Runtime dimensions (allow using a smaller mesh at run-time if needed) */
__attribute__((section(".hbm")))
static volatile uint32_t g_rt_num_clusters = NUM_CLUSTERS;
__attribute__((section(".hbm")))
static volatile uint32_t g_rt_cores_per_cluster = CORES_PER_CLUSTER;

static inline void soc_daml_set_runtime_dims(uint32_t num_clusters,
                                             uint32_t cores_per_cluster)
{
  if (num_clusters <= NUM_CLUSTERS) {
    g_rt_num_clusters = num_clusters;
  } else {
    g_rt_num_clusters = NUM_CLUSTERS;
  }

  if (cores_per_cluster <= CORES_PER_CLUSTER) {
    g_rt_cores_per_cluster = cores_per_cluster;
  } else {
    g_rt_cores_per_cluster = CORES_PER_CLUSTER;
  }
}

/* --------------------------
 * Tiny utils (no libc)
 * -------------------------- */
static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n_words) {
  uint32_t i;
  for (i = 0; i < n_words; i++) {
    p[i] = 0u;
  }
}
static inline void daml_zero_bytes(volatile void *ptr, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t*)ptr;
  uint32_t i;
  for (i = 0; i < n; i++) {
    q[i] = 0u;
  }
}
static inline void daml_fence(void) { __sync_synchronize(); }

/* --------------------------
 * Debug helpers
 * -------------------------- */
#ifndef BOOT_LOG
#define BOOT_LOG(...)   printf(__VA_ARGS__)
#endif
#ifndef SNAP_LOG
#define SNAP_LOG(...)   printf(__VA_ARGS__)
#endif

/* --------------------------
 * Arena math (per-core L1)
 * -------------------------- */
static inline void daml_compute_core_arena(uint32_t *out_base,
                                           uint32_t *out_size,
                                           uint32_t core_id)
{
  const uint32_t heap_base = (uint32_t)HEAP_BASE;
  const uint32_t heap_end  = (uint32_t)HEAP_END;
  const uint32_t heap_size = (heap_end > heap_base) ? (heap_end - heap_base) : 0u;

  uint32_t share;
  if (g_rt_cores_per_cluster > 0u) {
    share = heap_size / g_rt_cores_per_cluster;
  } else {
    share = 0u;
  }

  {
    /* Keep block alignment compatible with allocator’s MIN_BLOCK_SIZE */
    const uint32_t align = (uint32_t)sizeof(alloc_block_t);
    share = (share / align) * align;
  }

  {
    uint32_t base = heap_base + core_id * share;
    uint32_t size;

    if (core_id == (g_rt_cores_per_cluster - 1u)) {
      size = heap_end - base;
    } else {
      size = share;
    }

    {
      const uint32_t align = (uint32_t)sizeof(alloc_block_t);
      size = (size / align) * align;
    }

    *out_base = base;
    *out_size = size;
  }
}

/* ============================================================
 *                   SNAPSHOT + BITMAP HELPERS
 * ============================================================ */

/* Per-cluster allocator init (must be called on owner cluster) */
static inline void soc_daml_init_allocators_this_cluster(uint32_t cid)
{
  uint32_t k;

  /* Clear metadata for this cluster */
  g_hbm_cluster_common_count[cid] = 0u;
  daml_zero_bytes(&g_hbm_cluster_common[cid][0], (uint32_t)sizeof(g_hbm_cluster_common[cid]));
  for (k = 0; k < g_rt_cores_per_cluster; k++) {
    g_hbm_core_free_count[cid][k] = 0u;
    daml_zero_bytes(&g_hbm_core_free[cid][k][0], (uint32_t)sizeof(g_hbm_core_free[cid][k]));
    /* clear per-core bitmap too */
    daml_zero_u32(&g_hbm_core_free_bitmap[cid][k][0], BITMAP_WORDS);
  }
  daml_fence();

  /* Initialize the L1 allocator for each core of THIS cluster. */
  for (k = 0; k < g_rt_cores_per_cluster; k++) {
    uint32_t base, size;
    daml_compute_core_arena(&base, &size, k);
    flex_cluster_alloc_init(&g_hbm_l1_allocators[cid][k], (void*)base, size);

    BOOT_LOG("    [BOOT] C%u/K%u arena: [0x%08x .. 0x%08x) size=0x%08x\n",
             cid, k, base, base + size, size);
  }
}

/* Snapshot free list for (cid,kid) — must be called by the owner cluster */
static inline void soc_daml_upload_free_list(uint32_t cid, uint32_t kid)
{
  const uint32_t heap_lo = (uint32_t)HEAP_BASE;
  const uint32_t heap_hi = (uint32_t)HEAP_END;

  uint32_t count = 0u;
  alloc_t       *alloc;
  alloc_block_t *curr;

  SNAP_LOG("[SNAPSHOT] Enter C%u/K%u\n", cid, kid);

  g_hbm_core_free_count[cid][kid] = 0u;
  daml_fence();

  alloc = &g_hbm_l1_allocators[cid][kid];
  curr  = alloc->first_block;

  while ((curr != 0) && (count < MAX_FREE_BLOCKS_PER_CORE)) {
    uint32_t p = (uint32_t)curr;

    /* Validate pointer lies inside [heap_lo, heap_hi) for THIS cluster */
    if ((p < heap_lo) ||
        ((p + (uint32_t)sizeof(alloc_block_t)) > heap_hi) ||
        ((p & ((uint32_t)sizeof(alloc_block_t) - 1u)) != 0u)) {
      SNAP_LOG("[SNAPSHOT][WARN] C%u/K%u bad ptr=0x%08x (lo=0x%08x hi=0x%08x), abort list\n",
               cid, kid, p, heap_lo, heap_hi);
      break;
    }

    g_hbm_core_free[cid][kid][count].addr = (void*)p;
    g_hbm_core_free[cid][kid][count].size = curr->size;

    curr  = curr->next;
    count += 1u;
  }

  daml_fence();
  g_hbm_core_free_count[cid][kid] = count;

  SNAP_LOG("[SNAPSHOT] Uploaded C%u/K%u blocks=%u\n", cid, kid, count);
}

/* -------- Bitmap building for a (cluster,core) snapshot -------- */

static inline void daml_bitmap_clear_core(uint32_t cid, uint32_t kid)
{
  daml_zero_u32(&g_hbm_core_free_bitmap[cid][kid][0], BITMAP_WORDS);
}

/* Mark all slots overlapped by [addr, addr+size) as free in that core bitmap */
static inline void daml_bitmap_mark_range(uint32_t cid, uint32_t kid,
                                          uint32_t addr, uint32_t size)
{
  uint32_t heap_lo = (uint32_t)HEAP_BASE;
  uint32_t heap_hi = (uint32_t)HEAP_END;

  uint32_t start_addr;
  uint32_t end_addr;

  /* Clamp to heap */
  if (addr < heap_lo) {
    start_addr = heap_lo;
  } else {
    start_addr = addr;
  }

  {
    uint32_t lim = addr + size;
    if (lim > heap_hi) {
      end_addr = heap_hi;
    } else {
      end_addr = lim;
    }
  }

  if (end_addr <= start_addr) {
    return;
  }

  /* Compute covered slots [s0, s1] inclusive */
  {
    uint32_t off0 = start_addr - heap_lo;
    uint32_t off1 = end_addr   - heap_lo; /* end exclusive */

    uint32_t s0 = off0 / SLOT_SIZE;
    uint32_t s1;

    {
      uint32_t end_minus_1 = end_addr - 1u;
      uint32_t off1m1 = end_minus_1 - heap_lo;
      s1 = off1m1 / SLOT_SIZE;
    }

    if (s0 >= NUM_SLOTS) {
      return;
    }
    if (s1 >= NUM_SLOTS) {
      s1 = NUM_SLOTS - 1u;
    }

    /* Set bits for s in [s0, s1] */
    {
      uint32_t s;
      for (s = s0; s <= s1; s++) {
        uint32_t word_idx = s >> 5;
        uint32_t bit_idx  = s & 31u;
        uint32_t mask     = (1u << bit_idx);
        g_hbm_core_free_bitmap[cid][kid][word_idx] |= mask;
      }
    }
  }
}

/* Build per-core bitmap from its snapshot */
static inline void soc_daml_build_core_bitmap(uint32_t cid, uint32_t kid)
{
  uint32_t i;
  uint32_t n = g_hbm_core_free_count[cid][kid];

  daml_bitmap_clear_core(cid, kid);

  for (i = 0; i < n; i++) {
    uint32_t addr = (uint32_t)(uintptr_t)g_hbm_core_free[cid][kid][i].addr;
    uint32_t size = g_hbm_core_free[cid][kid][i].size;
    daml_bitmap_mark_range(cid, kid, addr, size);
  }
}

/* ============================================================
 *                BITMAP INTERSECTIONS + RANGE EMIT
 * ============================================================ */

/* Helper: set dst[] = 0xFFFFFFFF (and mask tail bits in last word) */
static inline void daml_bitmap_set_all_ones(uint32_t *dst_words)
{
  uint32_t w;
  for (w = 0; w < BITMAP_WORDS; w++) {
    dst_words[w] = 0xFFFFFFFFu;
  }
  /* Mask tail bits beyond NUM_SLOTS */
  {
    uint32_t tail_bits = NUM_SLOTS & 31u;
    if (tail_bits != 0u) {
      uint32_t valid_mask = (1u << tail_bits);
      valid_mask = valid_mask - 1u; /* low tail_bits = 1, others 0 */
      dst_words[BITMAP_WORDS - 1u] &= valid_mask;
    }
  }
}

/* dst = AND over all cores in cluster cid */
static inline void soc_daml_build_cluster_common_bitmap(uint32_t cid)
{
  uint32_t w;
  uint32_t k;

  /* Start with all-ones */
  for (w = 0; w < BITMAP_WORDS; w++) {
    g_hbm_cluster_common_bitmap[cid][w] = 0xFFFFFFFFu;
  }
  {
    uint32_t tail_bits = NUM_SLOTS & 31u;
    if (tail_bits != 0u) {
      uint32_t valid_mask = (1u << tail_bits);
      valid_mask = valid_mask - 1u;
      g_hbm_cluster_common_bitmap[cid][BITMAP_WORDS - 1u] &= valid_mask;
    }
  }

  /* AND across core bitmaps */
  for (k = 0; k < g_rt_cores_per_cluster; k++) {
    for (w = 0; w < BITMAP_WORDS; w++) {
      uint32_t word = g_hbm_core_free_bitmap[cid][k][w];
      g_hbm_cluster_common_bitmap[cid][w] &= word;
    }
  }
}

/* dst = AND over all clusters' cluster-common bitmaps */
static inline void soc_daml_build_system_common_bitmap(void)
{
  uint32_t w;
  uint32_t c;

  daml_bitmap_set_all_ones(&g_hbm_system_common_bitmap[0]);

  for (c = 0; c < g_rt_num_clusters; c++) {
    for (w = 0; w < BITMAP_WORDS; w++) {
      uint32_t word = g_hbm_cluster_common_bitmap[c][w];
      g_hbm_system_common_bitmap[w] &= word;
    }
  }
}

/* Convert a bitmap to (addr,size) ranges for cluster cid */
static inline void soc_daml_emit_cluster_ranges_from_bitmap(uint32_t cid)
{
  uint32_t out_cnt = 0u;
  uint32_t slot = 0u;

  g_hbm_cluster_common_count[cid] = 0u;
  daml_zero_bytes(&g_hbm_cluster_common[cid][0], (uint32_t)sizeof(g_hbm_cluster_common[cid]));

  while ((slot < NUM_SLOTS) && (out_cnt < MAX_COMMON_BLOCKS)) {
    /* find next set bit */
    uint32_t found = 0u;
    uint32_t start_slot = 0u;

    while (slot < NUM_SLOTS) {
      uint32_t word_idx = slot >> 5;
      uint32_t bit_idx  = slot & 31u;
      uint32_t mask     = (1u << bit_idx);
      uint32_t word     = g_hbm_cluster_common_bitmap[cid][word_idx];
      if ((word & mask) != 0u) {
        found = 1u;
        start_slot = slot;
        break;
      }
      slot = slot + 1u;
    }

    if (found == 0u) {
      break;
    }

    /* extend run */
    {
      uint32_t end_slot = start_slot;
      uint32_t cont = 1u;

      while ((cont == 1u) && (end_slot + 1u < NUM_SLOTS)) {
        uint32_t next = end_slot + 1u;
        uint32_t widx = next >> 5;
        uint32_t bidx = next & 31u;
        uint32_t m    = (1u << bidx);
        uint32_t wval = g_hbm_cluster_common_bitmap[cid][widx];
        if ((wval & m) != 0u) {
          end_slot = next;
        } else {
          cont = 0u;
        }
      }

      /* emit range */
      {
        uint32_t start_addr = (uint32_t)HEAP_BASE + start_slot * SLOT_SIZE;
        uint32_t end_addr   = (uint32_t)HEAP_BASE + (end_slot + 1u) * SLOT_SIZE;
        g_hbm_cluster_common[cid][out_cnt].addr = (void*)(uintptr_t)start_addr;
        g_hbm_cluster_common[cid][out_cnt].size = end_addr - start_addr;
        out_cnt = out_cnt + 1u;
      }

      slot = end_slot + 1u;
    }
  }

  g_hbm_cluster_common_count[cid] = out_cnt;
}

/* Convert the system common bitmap into ranges */
static inline void soc_daml_emit_system_ranges_from_bitmap(void)
{
  uint32_t out_cnt = 0u;
  uint32_t slot = 0u;

  g_hbm_system_common_count = 0u;
  daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));

  while ((slot < NUM_SLOTS) && (out_cnt < MAX_COMMON_BLOCKS)) {
    uint32_t found = 0u;
    uint32_t start_slot = 0u;

    while (slot < NUM_SLOTS) {
      uint32_t word_idx = slot >> 5;
      uint32_t bit_idx  = slot & 31u;
      uint32_t mask     = (1u << bit_idx);
      uint32_t word     = g_hbm_system_common_bitmap[word_idx];
      if ((word & mask) != 0u) {
        found = 1u;
        start_slot = slot;
        break;
      }
      slot = slot + 1u;
    }

    if (found == 0u) {
      break;
    }

    {
      uint32_t end_slot = start_slot;
      uint32_t cont = 1u;

      while ((cont == 1u) && (end_slot + 1u < NUM_SLOTS)) {
        uint32_t next = end_slot + 1u;
        uint32_t widx = next >> 5;
        uint32_t bidx = next & 31u;
        uint32_t m    = (1u << bidx);
        uint32_t wval = g_hbm_system_common_bitmap[widx];
        if ((wval & m) != 0u) {
          end_slot = next;
        } else {
          cont = 0u;
        }
      }

      {
        uint32_t start_addr = (uint32_t)HEAP_BASE + start_slot * SLOT_SIZE;
        uint32_t end_addr   = (uint32_t)HEAP_BASE + (end_slot + 1u) * SLOT_SIZE;
        g_hbm_system_common[out_cnt].addr = (void*)(uintptr_t)start_addr;
        g_hbm_system_common[out_cnt].size = end_addr - start_addr;
        out_cnt = out_cnt + 1u;
      }

      slot = end_slot + 1u;
    }
  }

  g_hbm_system_common_count = out_cnt;
}

/* ============================================================
 *              PUBLIC ENTRY POINTS (bitmap workflow)
 * ============================================================ */

/* Build per-core bitmaps from snapshots — call after snapshots are uploaded */
static inline void soc_daml_build_all_core_bitmaps(void)
{
  uint32_t c;
  uint32_t k;
  for (c = 0; c < g_rt_num_clusters; c++) {
    for (k = 0; k < g_rt_cores_per_cluster; k++) {
      soc_daml_build_core_bitmap(c, k);
    }
  }
}

/* Build cluster-common bitmaps + convert to ranges */
static inline void soc_daml_build_all_cluster_common_from_bitmaps(void)
{
  uint32_t c;
  for (c = 0; c < g_rt_num_clusters; c++) {
    soc_daml_build_cluster_common_bitmap(c);
    soc_daml_emit_cluster_ranges_from_bitmap(c);
  }
}

/* Build system-common bitmap + convert to ranges */
static inline void soc_daml_build_system_common_from_bitmaps(void)
{
  soc_daml_build_system_common_bitmap();
  soc_daml_emit_system_ranges_from_bitmap();
}

/* --------------------------
 * Getters (unchanged API)
 * -------------------------- */
static inline uint32_t soc_daml_get_core_free_count(uint32_t cid, uint32_t kid) {
  if (cid < NUM_CLUSTERS && kid < CORES_PER_CLUSTER) {
    return g_hbm_core_free_count[cid][kid];
  }
  return 0u;
}
static inline const daml_block_t* soc_daml_get_core_free(uint32_t cid, uint32_t kid) {
  if (cid < NUM_CLUSTERS && kid < CORES_PER_CLUSTER) {
    return &g_hbm_core_free[cid][kid][0];
  }
  return (const daml_block_t*)0;
}
static inline uint32_t soc_daml_get_cluster_common_count(uint32_t cid) {
  if (cid < NUM_CLUSTERS) {
    return g_hbm_cluster_common_count[cid];
  }
  return 0u;
}
static inline const daml_block_t* soc_daml_get_cluster_common(uint32_t cid) {
  if (cid < NUM_CLUSTERS) {
    return &g_hbm_cluster_common[cid][0];
  }
  return (const daml_block_t*)0;
}
static inline uint32_t soc_daml_get_system_common_count(void) {
  return g_hbm_system_common_count;
}
static inline const daml_block_t* soc_daml_get_system_common(void) {
  return &g_hbm_system_common[0];
}
