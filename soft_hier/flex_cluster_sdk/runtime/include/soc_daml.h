#pragma once
/*
 * soc_daml.h — SoftHier P1 runtime (HBM-visible allocation metadata)
 *
 * - Uses platform macros from flex_cluster_arch.h
 * - Per-core upload of L1 free-list snapshots into HBM
 * - Cluster-wide + system-wide intersections (free in ALL cores/clusters)
 * - No libc calls (no memset/memcpy/etc.)
 * - Spinlocks are NO-OPs (barriers serialize us already)
 */

#include <stdint.h>
#include <stddef.h>

#include "flex_cluster_arch.h"  // generated from flex_cluster_arch.py
#include "flex_alloc.h"         // alloc_t, alloc_block_t, flex_cluster_alloc_init(), domain_malloc(), domain_free()
#include "flex_printf.h"        // printf()

/* ------------------------------------------------------------
 *                 Dimensions (from platform config)
 * ------------------------------------------------------------ */
#ifndef ARCH_NUM_CLUSTER_X
#error "ARCH_NUM_CLUSTER_X not defined. Include generated flex_cluster_arch.h"
#endif
#ifndef ARCH_NUM_CLUSTER_Y
#error "ARCH_NUM_CLUSTER_Y not defined. Include generated flex_cluster_arch.h"
#endif
#ifndef ARCH_NUM_CORE_PER_CLUSTER
#error "ARCH_NUM_CORE_PER_CLUSTER not defined. Include generated flex_cluster_arch.h"
#endif
#ifndef ARCH_CLUSTER_HEAP_BASE
#error "ARCH_CLUSTER_HEAP_BASE not defined. Update flex_cluster_arch.py and rebuild."
#endif
#ifndef ARCH_CLUSTER_HEAP_END
#error "ARCH_CLUSTER_HEAP_END not defined. Update flex_cluster_arch.py and rebuild."
#endif
#ifndef ARCH_CLUSTER_TCDM_BASE
#error "ARCH_CLUSTER_TCDM_BASE not defined. Include generated flex_cluster_arch.h"
#endif
#ifndef ARCH_CLUSTER_TCDM_SIZE
#error "ARCH_CLUSTER_TCDM_SIZE not defined. Include generated flex_cluster_arch.h"
#endif

#define NUM_CLUSTER_X        (ARCH_NUM_CLUSTER_X)
#define NUM_CLUSTER_Y        (ARCH_NUM_CLUSTER_Y)
#define NUM_CLUSTERS         ((NUM_CLUSTER_X) * (NUM_CLUSTER_Y))
#define CORES_PER_CLUSTER    (ARCH_NUM_CORE_PER_CLUSTER)

/* Tweak these if you expect longer free lists or more results */
#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE  128u
#endif
#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS         128u
#endif

/* ------------------------------------------------------------
 *                         Types
 * ------------------------------------------------------------ */
typedef struct {
  void    *addr;   /* start address of the free block (block header address) */
  uint32_t size;   /* size in bytes of the free block */
} daml_block_t;

/* ------------------------------------------------------------
 *               HBM-resident global state (metadata)
 * ------------------------------------------------------------ */
__attribute__((section(".hbm")))
alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

__attribute__((section(".hbm")))
daml_block_t g_hbm_core_free[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_count[NUM_CLUSTERS][CORES_PER_CLUSTER];

__attribute__((section(".hbm")))
daml_block_t g_hbm_cluster_common[NUM_CLUSTERS][MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_cluster_common_count[NUM_CLUSTERS];

__attribute__((section(".hbm")))
daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_count;

/* left here in case we re-enable locks later */
__attribute__((section(".hbm")))
volatile int g_cluster_lock[NUM_CLUSTERS];

__attribute__((section(".hbm")))
volatile int g_global_lock;

/* ------------------------------------------------------------
 *           Runtime dimensions (for safe bounds/iteration)
 * ------------------------------------------------------------ */
__attribute__((section(".hbm")))
static volatile uint32_t g_rt_num_clusters = NUM_CLUSTERS;

__attribute__((section(".hbm")))
static volatile uint32_t g_rt_cores_per_cluster = CORES_PER_CLUSTER;

static inline void soc_daml_set_runtime_dims(uint32_t num_clusters,
                                             uint32_t cores_per_cluster)
{
  g_rt_num_clusters      = (num_clusters  <= NUM_CLUSTERS)      ? num_clusters      : NUM_CLUSTERS;
  g_rt_cores_per_cluster = (cores_per_cluster <= CORES_PER_CLUSTER) ? cores_per_cluster : CORES_PER_CLUSTER;
}

/* ------------------------------------------------------------
 *                     Tiny utils (no libc)
 * ------------------------------------------------------------ */
static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n) {
  for (uint32_t i = 0; i < n; ++i) p[i] = 0u;
}
static inline void daml_zero_bytes(volatile void *ptr, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t*)ptr;
  for (uint32_t i = 0; i < n; ++i) q[i] = 0u;
}
static inline void daml_fence(void) { __sync_synchronize(); }

static inline int daml_block_eq(const daml_block_t *a, const daml_block_t *b) {
  return (a->addr == b->addr) && (a->size == b->size);
}
static inline int daml_contains(const daml_block_t *list, uint32_t n, const daml_block_t *x) {
  for (uint32_t i = 0; i < n; ++i) if (daml_block_eq(&list[i], x)) return 1;
  return 0;
}

/* ------------------------------------------------------------
 *                         Spinlocks
 * ------------------------------------------------------------ */
/* IMPORTANT:
 * These used to use GCC atomics. On this platform, that can deadlock in GVSoC.
 * We serialize with global barriers instead, so locks are NO-OPs.
 */
static inline void daml_lock_cluster(uint32_t cid)   { (void)cid; }
static inline void daml_unlock_cluster(uint32_t cid) { (void)cid; }
static inline void daml_lock_global(void)            { }
static inline void daml_unlock_global(void)          { }

/* ------------------------------------------------------------
 *               Bounds guards (use runtime dims)
 * ------------------------------------------------------------ */
static inline int daml_in_bounds_cluster(int cid) {
  return ((unsigned)cid < NUM_CLUSTERS) && ((unsigned)cid < g_rt_num_clusters);
}
static inline int daml_in_bounds_cc(int cid, int core) {
  return ((unsigned)cid  < NUM_CLUSTERS)         &&
         ((unsigned)core < CORES_PER_CLUSTER)    &&
         ((unsigned)cid  < g_rt_num_clusters)    &&
         ((unsigned)core < g_rt_cores_per_cluster);
}

/* ------------------------------------------------------------
 *              Helper: derive per-core L1 arena
 * ------------------------------------------------------------ */
/* We expose one allocator per-core, but TCDM is shared per cluster.
 * To avoid overlaps, we split [HEAP_BASE, HEAP_END) evenly across cores.
 * NOTE: This is only for test/demo; production should use a single
 * per-cluster allocator and arbitrate in software.
 */
static inline void daml_compute_core_arena(uint32_t *out_base, uint32_t *out_size,
                                           uint32_t core_id)
{
  const uint32_t heap_base = (uint32_t)ARCH_CLUSTER_HEAP_BASE;
  const uint32_t heap_end  = (uint32_t)ARCH_CLUSTER_HEAP_END;
  const uint32_t heap_span = (heap_end > heap_base) ? (heap_end - heap_base) : 0;

  uint32_t slice = (heap_span / CORES_PER_CLUSTER);
  uint32_t base  = heap_base + core_id * slice;

  /* last core eats the remainder to cover to end */
  if (core_id == (CORES_PER_CLUSTER - 1)) {
    slice = heap_end - base;
  }

  *out_base = base;
  *out_size = slice;
}

/* ------------------------------------------------------------
 *                 Initialization (allocators + HBM)
 * ------------------------------------------------------------ */
static inline void soc_daml_init_allocators(void)
{
  /* zero global metadata */
  for (uint32_t c = 0; c < NUM_CLUSTERS; ++c) {
    g_cluster_lock[c] = 0;
    g_hbm_cluster_common_count[c] = 0u;
    daml_zero_bytes(&g_hbm_cluster_common[c][0], (uint32_t)sizeof(g_hbm_cluster_common[c]));
    for (uint32_t k = 0; k < CORES_PER_CLUSTER; ++k) {
      g_hbm_core_free_count[c][k] = 0u;
      daml_zero_bytes(&g_hbm_core_free[c][k][0], (uint32_t)sizeof(g_hbm_core_free[c][k]));
    }
  }
  g_global_lock = 0;
  g_hbm_system_common_count = 0u;
  daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
  daml_fence();

  /* init per-core allocators in HBM pointing into cluster L1 arenas */
  for (uint32_t cid = 0; cid < g_rt_num_clusters; ++cid) {
    printf("[BOOT] Init allocators (cluster %u)\n", cid);
    for (uint32_t kid = 0; kid < g_rt_cores_per_cluster; ++kid) {
      uint32_t base, size;
      daml_compute_core_arena(&base, &size, kid);

      /* sanity clamp inside TCDM range */
      const uint32_t tcdm_lo = (uint32_t)ARCH_CLUSTER_TCDM_BASE;
      const uint32_t tcdm_hi = tcdm_lo + (uint32_t)ARCH_CLUSTER_TCDM_SIZE;
      if (base < tcdm_lo) base = tcdm_lo;
      if (base + size > tcdm_hi) size = (tcdm_hi > base) ? (tcdm_hi - base) : 0;

      flex_cluster_alloc_init(&g_hbm_l1_allocators[cid][kid], (void*)base, size);

      printf("    [BOOT] C%u/K%u arena: [0x%08x .. 0x%08x) size=0x%08x\n",
             cid, kid, base, base + size, size);
    }
    printf("[BOOT] Done (cluster %u)\n", cid);
  }
  daml_fence();
}

/* ------------------------------------------------------------
 *                Per-core snapshot upload (P1)
 * ------------------------------------------------------------ */
static inline void soc_daml_upload_free_list(int cluster_id, int core_id)
{
  if (!daml_in_bounds_cc(cluster_id, core_id)) return;

  printf("[SNAPSHOT] Enter C%u/K%u\n", cluster_id, core_id);

  alloc_t       *alloc = &g_hbm_l1_allocators[cluster_id][core_id];
  alloc_block_t *curr  = alloc->first_block;   /* free list head (in L1) */
  uint32_t count = 0u;

  daml_lock_cluster(cluster_id);

  g_hbm_core_free_count[cluster_id][core_id] = 0u;  /* readers see empty until table is ready */
  daml_fence();

  while (curr && (count < MAX_FREE_BLOCKS_PER_CORE)) {
    g_hbm_core_free[cluster_id][core_id][count].addr = (void*)curr;
    g_hbm_core_free[cluster_id][core_id][count].size = curr->size;
    curr  = curr->next;
    count += 1u;
  }

  daml_fence();  /* publish entries before count */
  g_hbm_core_free_count[cluster_id][core_id] = count;

  daml_unlock_cluster(cluster_id);

  printf("[SNAPSHOT] Uploaded C%u/K%u blocks=%u\n", cluster_id, core_id, count);
}

/* ------------------------------------------------------------
 *            Cluster-wide intersection (free in ALL cores)
 * ------------------------------------------------------------ */
static inline void soc_daml_build_cluster_common(int cluster_id)
{
  if (!daml_in_bounds_cluster(cluster_id)) return;

  printf("[INTERSECT] Cluster %u begin\n", cluster_id);

  uint32_t out_cnt = 0u;

  daml_lock_cluster(cluster_id);

  g_hbm_cluster_common_count[cluster_id] = 0u;
  daml_fence();

  const daml_block_t *ref   = &g_hbm_core_free[cluster_id][0][0];
  const uint32_t      ref_n = g_hbm_core_free_count[cluster_id][0];

  for (uint32_t i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (uint32_t k = 1; k < g_rt_cores_per_cluster; ++k) {
      const daml_block_t *lst   = &g_hbm_core_free[cluster_id][k][0];
      const uint32_t      lst_n = g_hbm_core_free_count[cluster_id][k];
      if (!daml_contains(lst, lst_n, X)) { in_all = 0; break; }
    }

    if (in_all) {
      g_hbm_cluster_common[cluster_id][out_cnt] = *X;
      out_cnt += 1u;
    }
  }

  daml_fence();
  g_hbm_cluster_common_count[cluster_id] = out_cnt;

  daml_unlock_cluster(cluster_id);

  printf("[INTERSECT] Cluster %u done, common=%u\n", cluster_id, out_cnt);
}

/* ------------------------------------------------------------
 *           System-wide intersection (ALL clusters)
 * ------------------------------------------------------------ */
static inline void soc_daml_build_system_common(void)
{
  printf("[INTERSECT] System begin\n");

  uint32_t out_cnt = 0u;

  daml_lock_global();

  g_hbm_system_common_count = 0u;
  daml_fence();

  if (g_rt_num_clusters == 0u) { daml_unlock_global(); printf("[INTERSECT] System none\n"); return; }

  const daml_block_t *ref   = &g_hbm_cluster_common[0][0];
  const uint32_t      ref_n = g_hbm_cluster_common_count[0];

  for (uint32_t i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (uint32_t c = 1; c < g_rt_num_clusters; ++c) {
      const daml_block_t *lst   = &g_hbm_cluster_common[c][0];
      const uint32_t      lst_n = g_hbm_cluster_common_count[c];
      if (!daml_contains(lst, lst_n, X)) { in_all = 0; break; }
    }

    if (in_all) {
      g_hbm_system_common[out_cnt] = *X;
      out_cnt += 1u;
    }
  }

  daml_fence();
  g_hbm_system_common_count = out_cnt;

  daml_unlock_global();

  printf("[INTERSECT] System done, common=%u\n", out_cnt);
}

/* ------------------------------------------------------------
 *            Optional wrappers around L1 alloc/free
 * ------------------------------------------------------------ */
static inline void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size)
{
  if (!daml_in_bounds_cc(cluster_id, core_id)) return 0;

  void *ptr;
  daml_lock_cluster(cluster_id);
  ptr = domain_malloc(&g_hbm_l1_allocators[cluster_id][core_id], size);
  daml_fence();
  daml_unlock_cluster(cluster_id);

  soc_daml_upload_free_list(cluster_id, core_id);  /* publish fresh snapshot */
  return ptr;
}

static inline void flex_l1_block_free(int cluster_id, int core_id, void *ptr)
{
  if (!daml_in_bounds_cc(cluster_id, core_id)) return;

  daml_lock_cluster(cluster_id);
  domain_free(&g_hbm_l1_allocators[cluster_id][core_id], ptr);
  daml_fence();
  daml_unlock_cluster(cluster_id);

  soc_daml_upload_free_list(cluster_id, core_id);  /* publish fresh snapshot */
}

/* ------------------------------------------------------------
 *                       Public getters
 * ------------------------------------------------------------ */
static inline uint32_t soc_daml_get_core_free_count(int cluster_id, int core_id) {
  return daml_in_bounds_cc(cluster_id, core_id) ? g_hbm_core_free_count[cluster_id][core_id] : 0u;
}
static inline const daml_block_t* soc_daml_get_core_free_list(int cluster_id, int core_id) {
  return daml_in_bounds_cc(cluster_id, core_id) ? &g_hbm_core_free[cluster_id][core_id][0] : (const daml_block_t*)0;
}
static inline uint32_t soc_daml_get_cluster_common_count(int cluster_id) {
  return daml_in_bounds_cluster(cluster_id) ? g_hbm_cluster_common_count[cluster_id] : 0u;
}
static inline const daml_block_t* soc_daml_get_cluster_common(int cluster_id) {
  return daml_in_bounds_cluster(cluster_id) ? &g_hbm_cluster_common[cluster_id][0] : (const daml_block_t*)0;
}
static inline uint32_t soc_daml_get_system_common_count(void) {
  return g_hbm_system_common_count;
}
static inline const daml_block_t* soc_daml_get_system_common(void) {
  return &g_hbm_system_common[0];
}
