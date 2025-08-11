#pragma once
/*
 * soc_daml.h — HBM-visible allocator metadata, bitmap helpers,
 *              single-node HBM allocator bring-up, and broadcast control.
 *
 * - Cluster-local init for L1 allocators (no remote writes).
 * - Snapshots local L1 free lists and builds bitmaps per core.
 * - OR bitmaps across cores -> cluster-free; AND across clusters -> system-common.
 * - HBM allocator init sized to current build:
 *       base = ARCH_HBM_START_BASE
 *       size = ARCH_HBM_NODE_ADDR_SPACE * ARCH_NUM_NODE_PER_CTRL
 *   Optionally uses linker symbols __hbm_heap_start/__hbm_heap_end if present.
 * - Broadcast control in HBM: publish-go flags and per-cluster verify status.
 *
 * No libc calls; tiny zero/fence helpers are used. printf allowed for boot logs.
 */

#include <stdint.h>
#include <stddef.h>
#include "flex_cluster_arch.h"
#include "flex_runtime.h"
#include "flex_alloc.h"
#include "flex_printf.h"

/* ============================================================
 *                   Platform dimensions
 * ============================================================ */
#if defined(ARCH_NUM_CLUSTER)
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER)
#else
#define NUM_CLUSTERS         (ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y)
#endif
#define CORES_PER_CLUSTER    (ARCH_NUM_CORE_PER_CLUSTER)

/* Same numeric heap window in every cluster’s TCDM */
#define HEAP_BASE   (ARCH_CLUSTER_HEAP_BASE)
#define HEAP_END    (ARCH_CLUSTER_HEAP_END)

/* Slot granularity for the common-free test (bitmap approach) */
#ifndef SLOT_SIZE
#define SLOT_SIZE   (0x400u)   /* 1 KiB */
#endif

/* Derived slot counts for one cluster’s L1 heap */
#define L1_HEAP_SIZE_BYTES   ((uint32_t)((uint32_t)(HEAP_END) - (uint32_t)(HEAP_BASE)))
#define L1_SLOT_COUNT        (L1_HEAP_SIZE_BYTES / SLOT_SIZE)
#define L1_BITMAP_WORDS      ((L1_SLOT_COUNT + 31u) / 32u)

/* Tunables for list-based outputs (used to print ranges from bitmaps) */
#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS    128u
#endif
#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE  128u
#endif

/* ============================================================
 *                           Types
 * ============================================================ */
typedef struct {
  void    *addr;   /* L1 address (begin of range) */
  uint32_t size;   /* bytes */
} daml_block_t;

/* ============================================================
 *                   HBM-resident global state
 * ============================================================ */
__attribute__((section(".hbm")))
alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_count[NUM_CLUSTERS][CORES_PER_CLUSTER];

__attribute__((section(".hbm")))
daml_block_t g_hbm_core_free[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

/* Bitmap per (cluster,core); 1 bit = slot FREE in that core’s L1 */
__attribute__((section(".hbm")))
uint32_t g_core_free_bitmap[NUM_CLUSTERS][CORES_PER_CLUSTER][L1_BITMAP_WORDS];

/* Bitmap per cluster; OR across cores (slot FREE in any core of the cluster) */
__attribute__((section(".hbm")))
uint32_t g_cluster_free_bitmap[NUM_CLUSTERS][L1_BITMAP_WORDS];

/* System-common bitmap; AND across clusters (slot FREE in all clusters) */
__attribute__((section(".hbm")))
uint32_t g_system_common_bitmap[L1_BITMAP_WORDS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_cluster_common_count[NUM_CLUSTERS];

__attribute__((section(".hbm")))
daml_block_t g_hbm_cluster_common[NUM_CLUSTERS][MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_count;

__attribute__((section(".hbm")))
daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

/* Broadcast control (HBM-visible so all clusters see it) */
__attribute__((section(".hbm"))) volatile uint32_t g_bcast_go       = 0u;
__attribute__((section(".hbm"))) volatile uint32_t g_bcast_len      = 0u;
__attribute__((section(".hbm"))) volatile uint32_t g_bcast_src_addr = 0u;

/* Per-cluster verification status:
 *  -2 = unset, -1 = OK, >=0 = mismatch index (word offset) */
__attribute__((section(".hbm"))) volatile int g_verify_status[NUM_CLUSTERS];

/* Simple locks in HBM (kept for future use) */
__attribute__((section(".hbm")))
volatile int g_cluster_lock[NUM_CLUSTERS];

__attribute__((section(".hbm")))
volatile int g_global_lock;

/* ============================================================
 *                 Runtime dimensions (bounds)
 * ============================================================ */
__attribute__((section(".hbm")))
static volatile uint32_t g_rt_num_clusters = NUM_CLUSTERS;

__attribute__((section(".hbm")))
static volatile uint32_t g_rt_cores_per_cluster = CORES_PER_CLUSTER;

static inline void soc_daml_set_runtime_dims(uint32_t num_clusters,
                                             uint32_t cores_per_cluster)
{
  g_rt_num_clusters      = (num_clusters      <= NUM_CLUSTERS)      ? num_clusters      : NUM_CLUSTERS;
  g_rt_cores_per_cluster = (cores_per_cluster <= CORES_PER_CLUSTER) ? cores_per_cluster : CORES_PER_CLUSTER;
}

/* ============================================================
 *                     Tiny utils (no libc)
 * ============================================================ */
static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n) {
  uint32_t i; for (i = 0; i < n; ++i) p[i] = 0u;
}
static inline void daml_zero_bytes(volatile void *ptr, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t*)ptr;
  uint32_t i; for (i = 0; i < n; ++i) q[i] = 0u;
}
static inline void daml_fence(void) { __sync_synchronize(); }

/* ============================================================
 *                        Debug logs
 * ============================================================ */
#ifndef BOOT_LOG
#define BOOT_LOG(...)   printf(__VA_ARGS__)
#endif
#ifndef SNAP_LOG
#define SNAP_LOG(...)   printf(__VA_ARGS__)
#endif

/* ============================================================
 *                   Per-core L1 arena math
 * ============================================================ */
static inline void daml_compute_core_arena(uint32_t *out_base,
                                           uint32_t *out_size,
                                           uint32_t core_id)
{
  const uint32_t heap_base = (uint32_t)HEAP_BASE;
  const uint32_t heap_end  = (uint32_t)HEAP_END;
  const uint32_t heap_size = (heap_end > heap_base) ? (heap_end - heap_base) : 0u;

  uint32_t share = (g_rt_cores_per_cluster > 0u) ? (heap_size / g_rt_cores_per_cluster) : 0u;

  const uint32_t align = (uint32_t)sizeof(alloc_block_t);
  share = (share / align) * align;

  uint32_t base = heap_base + core_id * share;
  uint32_t size = (core_id == (g_rt_cores_per_cluster - 1u)) ? (heap_end - base) : share;
  size = (size / align) * align;

  *out_base = base;
  *out_size = size;
}

/* ============================================================
 *                 Per-cluster allocator init (L1)
 *  (must execute on the owning cluster)
 * ============================================================ */
static inline void soc_daml_init_allocators_this_cluster(uint32_t cid)
{
  g_hbm_cluster_common_count[cid] = 0u;
  daml_zero_bytes(&g_hbm_cluster_common[cid][0], (uint32_t)sizeof(g_hbm_cluster_common[cid]));

  uint32_t k;
  for (k = 0; k < g_rt_cores_per_cluster; ++k) {
    g_hbm_core_free_count[cid][k] = 0u;
    daml_zero_bytes(&g_hbm_core_free[cid][k][0], (uint32_t)sizeof(g_hbm_core_free[cid][k]));
    daml_zero_u32(&g_core_free_bitmap[cid][k][0], L1_BITMAP_WORDS);
  }
  daml_zero_u32(&g_cluster_free_bitmap[cid][0], L1_BITMAP_WORDS);
  daml_fence();

  for (k = 0; k < g_rt_cores_per_cluster; ++k) {
    uint32_t base, size;
    daml_compute_core_arena(&base, &size, k);
    flex_cluster_alloc_init(&g_hbm_l1_allocators[cid][k], (void*)(uintptr_t)base, size);

    BOOT_LOG("    [BOOT] C%u/K%u arena: [0x%08x .. 0x%08x) size=0x%08x\n",
             cid, k, base, base + size, size);
  }
}

/* ============================================================
 *                    Snapshot per core (L1)
 * ============================================================ */
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
    uint32_t p = (uint32_t)(uintptr_t)curr;

    if (p < heap_lo || (p + (uint32_t)sizeof(alloc_block_t)) > heap_hi || (p & (sizeof(alloc_block_t)-1u))) {
      SNAP_LOG("[SNAPSHOT][WARN] C%u/K%u bad ptr=0x%08x (lo=0x%08x hi=0x%08x)\n",
               cid, kid, p, heap_lo, heap_hi);
      break;
    }

    g_hbm_core_free[cid][kid][count].addr = (void*)(uintptr_t)p;
    g_hbm_core_free[cid][kid][count].size = curr->size;

    curr  = curr->next;
    count += 1u;
  }

  daml_fence();
  g_hbm_core_free_count[cid][kid] = count;

  SNAP_LOG("[SNAPSHOT] Uploaded C%u/K%u blocks=%u\n", cid, kid, count);
}

/* ============================================================
 *       Build per-core FREE bitmap from snapshot (L1)
 * ============================================================ */
static inline void soc_daml_build_core_bitmap(uint32_t cid, uint32_t kid)
{
  daml_zero_u32(&g_core_free_bitmap[cid][kid][0], L1_BITMAP_WORDS);

  uint32_t n = g_hbm_core_free_count[cid][kid];
  uint32_t i;
  for (i = 0; i < n; ++i) {
    uint32_t lo = (uint32_t)(uintptr_t)g_hbm_core_free[cid][kid][i].addr;
    uint32_t sz = g_hbm_core_free[cid][kid][i].size;

    uint32_t block_lo = (lo < (uint32_t)HEAP_BASE) ? (uint32_t)HEAP_BASE : lo;
    uint32_t block_hi = lo + sz;
    if (block_hi > (uint32_t)HEAP_END) block_hi = (uint32_t)HEAP_END;

    if (block_hi <= block_lo) continue;

    uint32_t first_slot = (block_lo - (uint32_t)HEAP_BASE) / SLOT_SIZE;
    uint32_t last_slot_excl = (block_hi - (uint32_t)HEAP_BASE + (SLOT_SIZE - 1u)) / SLOT_SIZE;

    if (last_slot_excl > L1_SLOT_COUNT) last_slot_excl = L1_SLOT_COUNT;

    uint32_t s;
    for (s = first_slot; s < last_slot_excl; ++s) {
      uint32_t word = s >> 5;
      uint32_t bit  = s & 31u;
      g_core_free_bitmap[cid][kid][word] |= (1u << bit);
    }
  }
}

/* ============================================================
 *            CLUSTER-FREE bitmap: OR across cores
 * ============================================================ */
static inline void soc_daml_build_cluster_free_bitmap(uint32_t cid)
{
  daml_zero_u32(&g_cluster_free_bitmap[cid][0], L1_BITMAP_WORDS);

  uint32_t w;
  for (w = 0; w < L1_BITMAP_WORDS; ++w) {
    uint32_t acc = 0u;
    uint32_t k;
    for (k = 0; k < g_rt_cores_per_cluster; ++k) {
      acc |= g_core_free_bitmap[cid][k][w];
    }
    g_cluster_free_bitmap[cid][w] = acc;
  }
}

/* ============================================================
 *     SYSTEM-COMMON bitmap: AND across all clusters
 * ============================================================ */
static inline void soc_daml_build_system_common_bitmap(void)
{
  uint32_t w;
  for (w = 0; w < L1_BITMAP_WORDS; ++w) {
    g_system_common_bitmap[w] = g_cluster_free_bitmap[0][w];
  }

  uint32_t c;
  for (c = 1u; c < g_rt_num_clusters; ++c) {
    for (w = 0; w < L1_BITMAP_WORDS; ++w) {
      g_system_common_bitmap[w] &= g_cluster_free_bitmap[c][w];
    }
  }
}

/* ============================================================
 *  Compress system-common bitmap into byte ranges (for print)
 * ============================================================ */
static inline void soc_daml_system_common_bitmap_to_ranges(void)
{
  g_hbm_system_common_count = 0u;

  uint32_t s = 0u;
  while (s < L1_SLOT_COUNT && g_hbm_system_common_count < MAX_COMMON_BLOCKS) {

    while (s < L1_SLOT_COUNT) {
      uint32_t w   = s >> 5;
      uint32_t bit = s & 31u;
      if ((g_system_common_bitmap[w] >> bit) & 1u) break;
      s += 1u;
    }
    if (s >= L1_SLOT_COUNT) break;

    uint32_t run_start = s;
    uint32_t e = s + 1u;
    while (e < L1_SLOT_COUNT) {
      uint32_t w2   = e >> 5;
      uint32_t bit2 = e & 31u;
      if (((g_system_common_bitmap[w2] >> bit2) & 1u) == 0u) break;
      e += 1u;
    }

    uint32_t base = (uint32_t)HEAP_BASE + run_start * SLOT_SIZE;
    uint32_t size = (e - run_start) * SLOT_SIZE;

    g_hbm_system_common[g_hbm_system_common_count].addr = (void*)(uintptr_t)base;
    g_hbm_system_common[g_hbm_system_common_count].size = size;
    g_hbm_system_common_count += 1u;

    s = e;
  }
}

/* ============================================================
 *        Simple getters (used by app)
 * ============================================================ */
static inline uint32_t soc_daml_get_system_common_count(void) {
  return g_hbm_system_common_count;
}
static inline const daml_block_t* soc_daml_get_system_common(void) {
  return &g_hbm_system_common[0];
}

/* ============================================================
 *         HBM allocator bring-up (SINGLE NODE)
 * ============================================================ */
extern volatile alloc_t alloc_hbm;

/* Match flex_runtime.h types (char[]) to avoid conflicts */
extern char __hbm_heap_start[];
extern char __hbm_heap_end[];

#ifndef SOC_DAML_HBM_USE_LINKER
#define SOC_DAML_HBM_USE_LINKER 1
#endif

static inline void soc_daml_init_hbm_allocator(void)
{
  uint32_t base = (uint32_t)ARCH_HBM_START_BASE;
  uint32_t end  = base + (uint32_t)ARCH_HBM_NODE_ADDR_SPACE * (uint32_t)ARCH_NUM_NODE_PER_CTRL;
  uint32_t size = end - base;

#if SOC_DAML_HBM_USE_LINKER
  uint32_t lbase = (uint32_t)(uintptr_t)__hbm_heap_start;
  uint32_t lend  = (uint32_t)(uintptr_t)__hbm_heap_end;
  if (lend > lbase && lbase != 0u) {
    base = lbase;
    end  = lend;
    size = end - base;
  }
#endif

  const uint32_t align = (uint32_t)sizeof(alloc_block_t);
  uint32_t aligned_base = (base + align - 1u) & ~(align - 1u);
  uint32_t aligned_size = (size - (aligned_base - base));
  aligned_size = (aligned_size / align) * align;

  if (aligned_size < (uint32_t)sizeof(alloc_block_t)) {
    printf("[HBM][ERR] No usable HBM heap (base=0x%08x size=0x%08x)\n", base, size);
    return;
  }

  flex_cluster_alloc_init((alloc_t*)&alloc_hbm, (void*)(uintptr_t)aligned_base, aligned_size);
  printf("[HBM] Heap: [0x%08x .. 0x%08x) size=0x%08x\n", aligned_base, aligned_base + aligned_size, aligned_size);
}

/* Thin wrappers */
static inline void* soc_daml_hbm_malloc(uint32_t size_bytes)
{
  return domain_malloc((alloc_t*)&alloc_hbm, size_bytes);
}
static inline void  soc_daml_hbm_free(void *ptr)
{
  domain_free((alloc_t*)&alloc_hbm, ptr);
}
