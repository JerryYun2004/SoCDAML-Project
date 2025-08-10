#pragma once
/*
 * soc_daml.h — SoftHier P1 runtime (HBM-visible allocation metadata)
 *
 * Self-contained (no external arch header required).
 * Defaults: 4x4 mesh, 3 cores/cluster  => NUM_CLUSTERS=16, CORES_PER_CLUSTER=3.
 * You can override at build-time: -DNUM_CLUSTERS=<N> -DCORES_PER_CLUSTER=<M>
 *
 * - Per-core upload of L1 free-list snapshots into HBM
 * - Cluster-wide + system-wide intersections (free in ALL cores[/clusters])
 * - Spinlocks + fences for correctness
 * - No compact libc calls (no memset/memcpy/etc.)
 */

#include <stdint.h>
#include <stddef.h>
#include "flex_alloc.h"   /* alloc_t, alloc_block_t, flex_cluster_alloc_init(), domain_malloc(), domain_free() */

/* ------------------------------------------------------------
 *                 Dimensions (self-contained)
 * ------------------------------------------------------------ */
/* Defaults for SoftHier tutorial: 4x4 clusters, 3 cores/cluster */
#ifndef NUM_CLUSTERS
#define NUM_CLUSTERS         16u
#endif
#ifndef CORES_PER_CLUSTER
#define CORES_PER_CLUSTER    3u
#endif

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

__attribute__((section(".hbm")))
volatile int g_cluster_lock[NUM_CLUSTERS];

__attribute__((section(".hbm")))
volatile int g_global_lock;

/* ------------------------------------------------------------
 *           Runtime dimensions (for safe bounds/iteration)
 * ------------------------------------------------------------ */
/* These are caps used by iteration/guards. You can set them from main()
 * via soc_daml_set_runtime_dims() (<= build-time caps above). */
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
static int daml_contains(const daml_block_t *list, uint32_t n, const daml_block_t *x) {
  for (uint32_t i = 0; i < n; ++i) if (daml_block_eq(&list[i], x)) return 1;
  return 0;
}

/* ------------------------------------------------------------
 *                         Spinlocks
 * ------------------------------------------------------------ */
static inline void daml_lock_cluster(int cid) {
  while (__sync_lock_test_and_set(&g_cluster_lock[cid], 1)) { /* spin */ }
}
static inline void daml_unlock_cluster(int cid) {
  __sync_lock_release(&g_cluster_lock[cid]);
}
static inline void daml_lock_global(void) {
  while (__sync_lock_test_and_set(&g_global_lock, 1)) { /* spin */ }
}
static inline void daml_unlock_global(void) {
  __sync_lock_release(&g_global_lock);
}

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
 *                 Initialization (allocators + HBM)
 * ------------------------------------------------------------ */
/* Pass per-(cluster,core) base+size for L1 allocators.
 * Note: allocator/free-list shape defined in flex_alloc.h (node at block start). */
static inline void soc_daml_init_allocators(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER],
                                            uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER])
{
  for (uint32_t c = 0; c < NUM_CLUSTERS; ++c) {
    for (uint32_t k = 0; k < CORES_PER_CLUSTER; ++k) {
      flex_cluster_alloc_init(&g_hbm_l1_allocators[c][k], base_addrs[c][k], sizes[c][k]);
    }
  }

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
  daml_fence();  /* publish zeros */
}

/* ------------------------------------------------------------
 *                Per-core snapshot upload (P1)
 * ------------------------------------------------------------ */
static inline void soc_daml_upload_free_list(int cluster_id, int core_id)
{
  if (!daml_in_bounds_cc(cluster_id, core_id)) return;

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
}

/* ------------------------------------------------------------
 *            Cluster-wide intersection (free in ALL cores)
 * ------------------------------------------------------------ */
static inline void soc_daml_build_cluster_common(int cluster_id)
{
  if (!daml_in_bounds_cluster(cluster_id)) return;

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
      if (k >= CORES_PER_CLUSTER) { in_all = 0; break; }  /* static cap too small */
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
}

/* ------------------------------------------------------------
 *           System-wide intersection (ALL clusters)
 * ------------------------------------------------------------ */
static inline void soc_daml_build_system_common(void)
{
  uint32_t out_cnt = 0u;

  daml_lock_global();

  g_hbm_system_common_count = 0u;
  daml_fence();

  if (g_rt_num_clusters == 0u) { daml_unlock_global(); return; }

  const daml_block_t *ref   = &g_hbm_cluster_common[0][0];
  const uint32_t      ref_n = (0 < NUM_CLUSTERS) ? g_hbm_cluster_common_count[0] : 0u;

  for (uint32_t i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (uint32_t c = 1; c < g_rt_num_clusters; ++c) {
      if (c >= NUM_CLUSTERS) { in_all = 0; break; }  /* static cap too small */
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
