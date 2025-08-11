#pragma once
/*
 * soc_daml.h — Cluster-level OR, system-level AND common-free detection
 *
 * Semantics:
 *   - A slot is FREE in a cluster if it is free in ANY core’s L1 arena (OR across cores).
 *   - A slot is system-common if it is FREE in EVERY cluster (AND across clusters).
 *
 * Pipeline:
 *   snapshots (lists of (addr,size) per (cluster,core))
 *   -> per-core FREE bitmaps (S=0x400)
 *   -> per-cluster FREE bitmap = OR over cores
 *   -> system-common bitmap = AND over clusters
 *   -> compress to contiguous ranges in g_hbm_system_common + count
 *
 * No libc; small helpers only.
 */

#include <stdint.h>
#include <stddef.h>
#include "flex_cluster_arch.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_printf.h"

/* --------------------------
 * Dimensions / Heap bounds
 * -------------------------- */
#if defined(ARCH_NUM_CLUSTER)
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER)
#else
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y)
#endif
#define CORES_PER_CLUSTER    (ARCH_NUM_CORE_PER_CLUSTER)

#define HEAP_BASE   (ARCH_CLUSTER_HEAP_BASE)
#define HEAP_END    (ARCH_CLUSTER_HEAP_END)

/* Slot size for bitmaps (in-between granularity) */
#ifndef SLOT_SIZE
#define SLOT_SIZE 0x400u   /* 1 KB slots */
#endif

/* Derived slot geometry */
#define HEAP_SIZE_BYTES   ((uint32_t)((uint32_t)HEAP_END - (uint32_t)HEAP_BASE))
#define NUM_SLOTS         (HEAP_SIZE_BYTES / SLOT_SIZE)
#define BITMAP_WORDS      ((NUM_SLOTS + 31u) / 32u)

/* Limits for list-style outputs (just for printing / API parity) */
#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE  128u
#endif
#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS         128u
#endif

/* --------------------------
 * Types
 * -------------------------- */
typedef struct {
  void    *addr;   /* block header (or slot) base */
  uint32_t size;   /* bytes */
} daml_block_t;

/* --------------------------
 * HBM-resident global state
 * -------------------------- */

/* Original per-(cluster,core) allocator handles (initialized on owner cluster) */
__attribute__((section(".hbm")))
alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

/* Original snapshots: free-list nodes per (cluster,core) */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_count[NUM_CLUSTERS][CORES_PER_CLUSTER];
__attribute__((section(".hbm")))
daml_block_t g_hbm_core_free[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

/* New: per-(cluster,core) FREE bitmaps (OR → cluster bitmap) */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_bitmap[NUM_CLUSTERS][CORES_PER_CLUSTER][BITMAP_WORDS];

/* New: per-cluster FREE bitmap = OR of cores */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_cluster_free_bitmap[NUM_CLUSTERS][BITMAP_WORDS];

/* New: system-common FREE bitmap = AND of clusters */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_bitmap[BITMAP_WORDS];

/* For convenience: compress final bitmap to ranges for logging / use */
__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_count;
__attribute__((section(".hbm")))
daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

/* Simple locks (unused in this single-threaded builder, but kept for parity) */
__attribute__((section(".hbm"))) volatile int g_cluster_lock[NUM_CLUSTERS];
__attribute__((section(".hbm"))) volatile int g_global_lock;

/* Runtime dims (if you ever choose to run a sub-mesh) */
__attribute__((section(".hbm"))) static volatile uint32_t g_rt_num_clusters = NUM_CLUSTERS;
__attribute__((section(".hbm"))) static volatile uint32_t g_rt_cores_per_cluster = CORES_PER_CLUSTER;

static inline void soc_daml_set_runtime_dims(uint32_t num_clusters,
                                             uint32_t cores_per_cluster)
{
  g_rt_num_clusters      = (num_clusters      <= NUM_CLUSTERS)      ? num_clusters      : NUM_CLUSTERS;
  g_rt_cores_per_cluster = (cores_per_cluster <= CORES_PER_CLUSTER) ? cores_per_cluster : CORES_PER_CLUSTER;
}

/* --------------------------
 * Tiny utils (no libc)
 * -------------------------- */
static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n_words) {
  for (uint32_t i = 0; i < n_words; ++i) p[i] = 0u;
}
static inline void daml_zero_bytes(volatile void *ptr, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t*)ptr;
  for (uint32_t i = 0; i < n; ++i) q[i] = 0u;
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
 * Arena math for per-core L1
 * -------------------------- */
static inline void daml_compute_core_arena(uint32_t *out_base,
                                           uint32_t *out_size,
                                           uint32_t core_id)
{
  const uint32_t heap_base = (uint32_t)HEAP_BASE;
  const uint32_t heap_end  = (uint32_t)HEAP_END;
  const uint32_t heap_size = (heap_end > heap_base) ? (heap_end - heap_base) : 0u;

  uint32_t share = (g_rt_cores_per_cluster > 0) ? heap_size / g_rt_cores_per_cluster : 0u;

  /* Align to allocator block header */
  const uint32_t align = (uint32_t)sizeof(alloc_block_t);
  share = (share / align) * align;

  uint32_t base = heap_base + core_id * share;
  uint32_t size = (core_id == (g_rt_cores_per_cluster - 1)) ? (heap_end - base) : share;
  size = (size / align) * align;

  *out_base = base;
  *out_size = size;
}

/* --------------------------
 * Cluster-local allocator init
 * -------------------------- */
static inline void soc_daml_init_allocators_this_cluster(uint32_t cid)
{
  /* clear old metadata (snapshots + bitmaps) */
  for (uint32_t k = 0; k < g_rt_cores_per_cluster; ++k) {
    g_hbm_core_free_count[cid][k] = 0u;
    daml_zero_bytes(&g_hbm_core_free[cid][k][0], (uint32_t)sizeof(g_hbm_core_free[cid][k]));
    daml_zero_u32(&g_hbm_core_free_bitmap[cid][k][0], BITMAP_WORDS);
  }
  daml_zero_u32(&g_hbm_cluster_free_bitmap[cid][0], BITMAP_WORDS);
  daml_fence();

  /* init per-core allocators for this cluster */
  for (uint32_t kid = 0; kid < g_rt_cores_per_cluster; ++kid) {
    uint32_t base, size;
    daml_compute_core_arena(&base, &size, kid);
    flex_cluster_alloc_init(&g_hbm_l1_allocators[cid][kid], (void*)base, size);
    BOOT_LOG("    [BOOT] C%u/K%u arena: [0x%08x .. 0x%08x) size=0x%08x\n",
             cid, kid, base, base + size, size);
  }
}

/* --------------------------
 * Upload free-list snapshot
 * -------------------------- */
static inline void soc_daml_upload_free_list(uint32_t cid, uint32_t kid)
{
  const uint32_t heap_lo = (uint32_t)HEAP_BASE;
  const uint32_t heap_hi = (uint32_t)HEAP_END;

  SNAP_LOG("[SNAPSHOT] Enter C%u/K%u\n", cid, kid);

  g_hbm_core_free_count[cid][kid] = 0u;
  daml_fence();

  alloc_t       *alloc = &g_hbm_l1_allocators[cid][kid];
  alloc_block_t *curr  = alloc->first_block;
  uint32_t count = 0u;

  while (curr && (count < MAX_FREE_BLOCKS_PER_CORE)) {
    uint32_t p = (uint32_t)curr;

    if (p < heap_lo || (p + (uint32_t)sizeof(alloc_block_t)) > heap_hi || (p & (sizeof(alloc_block_t)-1))) {
      SNAP_LOG("[SNAPSHOT][WARN] C%u/K%u bad ptr=0x%08x\n", cid, kid, p);
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

/* --------------------------
 * Bitmap helpers
 * -------------------------- */

/* Mark [start, start+size) as FREE in bitmap, quantized to SLOT_SIZE. */
static inline void daml_bitmap_mark_run(volatile uint32_t *bm,
                                        uint32_t start,
                                        uint32_t size)
{
  const uint32_t heap_lo = (uint32_t)HEAP_BASE;
  const uint32_t heap_hi = (uint32_t)HEAP_END;

  /* Clamp to heap */
  uint32_t lo = (start < heap_lo) ? heap_lo : start;
  uint32_t hi = (start + size > heap_hi) ? heap_hi : (start + size);
  if (hi <= lo) return;

  /* Slot indices */
  uint32_t first = (lo - heap_lo) / SLOT_SIZE;
  uint32_t last_excl = (hi - heap_lo + SLOT_SIZE - 1u) / SLOT_SIZE; /* ceil */
  if (last_excl > NUM_SLOTS) last_excl = NUM_SLOTS;

  /* Set bits [first, last_excl) */
  for (uint32_t s = first; s < last_excl; ++s) {
    uint32_t w = s >> 5;          /* /32 */
    uint32_t b = s & 31u;         /* %32 */
    bm[w] |= (1u << b);
  }
}

/* Build per-core FREE bitmap from that core's snapshot */
static inline void soc_daml_build_core_bitmap(uint32_t cid, uint32_t kid)
{
  volatile uint32_t *bm = &g_hbm_core_free_bitmap[cid][kid][0];
  daml_zero_u32(bm, BITMAP_WORDS);

  const uint32_t n = g_hbm_core_free_count[cid][kid];
  for (uint32_t i = 0; i < n; ++i) {
    const daml_block_t *blk = &g_hbm_core_free[cid][kid][i];
    uint32_t start = (uint32_t)(uintptr_t)blk->addr;
    uint32_t size  = blk->size;
    daml_bitmap_mark_run(bm, start, size);
  }

  /* mask tail bits in last word */
  if ((NUM_SLOTS & 31u) != 0u) {
    uint32_t tail = NUM_SLOTS & 31u;
    uint32_t mask = (1u << tail); mask = mask - 1u;
    bm[BITMAP_WORDS - 1u] &= mask;
  }
}

/* dst (cluster FREE) = OR over cores */
static inline void soc_daml_build_cluster_free_bitmap(uint32_t cid)
{
  /* zero */
  for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
    g_hbm_cluster_free_bitmap[cid][w] = 0u;
  }
  /* OR in every core */
  for (uint32_t k = 0; k < g_rt_cores_per_cluster; ++k) {
    for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
      g_hbm_cluster_free_bitmap[cid][w] |= g_hbm_core_free_bitmap[cid][k][w];
    }
  }
  /* mask tail */
  if ((NUM_SLOTS & 31u) != 0u) {
    uint32_t tail = NUM_SLOTS & 31u;
    uint32_t mask = (1u << tail); mask = mask - 1u;
    g_hbm_cluster_free_bitmap[cid][BITMAP_WORDS - 1u] &= mask;
  }
}

/* system-common = AND across clusters of cluster_free bitmaps */
static inline void soc_daml_build_system_common_bitmap(void)
{
  /* init with all-ones in valid bits */
  for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
    g_hbm_system_common_bitmap[w] = 0xFFFFFFFFu;
  }
  if ((NUM_SLOTS & 31u) != 0u) {
    uint32_t tail = NUM_SLOTS & 31u;
    uint32_t mask = (1u << tail); mask = mask - 1u;
    g_hbm_system_common_bitmap[BITMAP_WORDS - 1u] = mask; /* only valid bits set */
  }

  for (uint32_t c = 0; c < g_rt_num_clusters; ++c) {
    for (uint32_t w = 0; w < BITMAP_WORDS; ++w) {
      g_hbm_system_common_bitmap[w] &= g_hbm_cluster_free_bitmap[c][w];
    }
  }
}

/* Compress final bitmap into contiguous slot ranges in bytes */
static inline void soc_daml_system_common_bitmap_to_ranges(void)
{
  g_hbm_system_common_count = 0u;

  uint32_t idx = 0u;     /* slot index */
  while (idx < NUM_SLOTS && g_hbm_system_common_count < MAX_COMMON_BLOCKS) {
    /* seek next 1 */
    while (idx < NUM_SLOTS) {
      uint32_t w = idx >> 5;
      uint32_t b = idx & 31u;
      if ( (g_hbm_system_common_bitmap[w] >> b) & 1u ) break;
      idx++;
    }
    if (idx >= NUM_SLOTS) break;

    /* accumulate run of 1s */
    uint32_t run_start = idx;
    while (idx < NUM_SLOTS) {
      uint32_t w = idx >> 5;
      uint32_t b = idx & 31u;
      if ( ((g_hbm_system_common_bitmap[w] >> b) & 1u) == 0u ) break;
      idx++;
    }
    uint32_t run_end_excl = idx;

    /* convert to byte range */
    uint32_t byte_lo = (uint32_t)HEAP_BASE + run_start * SLOT_SIZE;
    uint32_t byte_hi = (uint32_t)HEAP_BASE + run_end_excl * SLOT_SIZE;

    g_hbm_system_common[g_hbm_system_common_count].addr = (void*)(uintptr_t)byte_lo;
    g_hbm_system_common[g_hbm_system_common_count].size = (byte_hi - byte_lo);
    g_hbm_system_common_count++;
  }
}

/* --------------------------
 * Getters
 * -------------------------- */
static inline uint32_t soc_daml_get_system_common_count(void) {
  return g_hbm_system_common_count;
}
static inline const daml_block_t* soc_daml_get_system_common(void) {
  return &g_hbm_system_common[0];
}
