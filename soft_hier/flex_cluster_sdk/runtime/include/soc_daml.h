#pragma once
/*
 * soc_daml.h  —  SoftHier runtime extensions for P1
 *
 * Features:
 *  - Per-core upload of L1 free-list snapshots to HBM
 *  - Cluster-wide & system-wide intersection of free blocks
 *  - No compact libc calls (no memset/memcpy/…)
 *  - Plain C spinlocks + memory fences for GVSoC
 *
 * External dependencies:
 *   - flex_alloc.h for alloc_t / alloc_block_t and domain_malloc/domain_free/flex_cluster_alloc_init
 */

#include <stdint.h>
#include <stddef.h>
#include "flex_alloc.h"   // project allocator API: alloc_t, alloc_block_t, flex_cluster_alloc_init(), domain_malloc(), domain_free()

/* ------------------------------------------------------------
 *               Tunables (set to your platform)
 * ------------------------------------------------------------ */
#ifndef NUM_CLUSTERS
#define NUM_CLUSTERS            2
#endif

#ifndef CORES_PER_CLUSTER
#define CORES_PER_CLUSTER       8
#endif

#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE 128
#endif

#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS       128
#endif

/* ------------------------------------------------------------
 *                   Types (HBM metadata)
 * ------------------------------------------------------------ */
typedef struct {
  void    *addr;     /* start address of the free block (block header address) */
  uint32_t size;     /* size in bytes of the free block */
} daml_block_t;

/* ------------------------------------------------------------
 *                HBM-resident global structures
 * ------------------------------------------------------------ */
/* Per-core L1 allocators (mirror / manager-owned instances) */
__attribute__((section(".hbm")))
alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

/* Per-(cluster,core) free-list snapshots */
__attribute__((section(".hbm")))
daml_block_t g_hbm_core_free[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_core_free_count[NUM_CLUSTERS][CORES_PER_CLUSTER];

/* Per-cluster: blocks free in ALL cores of the cluster */
__attribute__((section(".hbm")))
daml_block_t g_hbm_cluster_common[NUM_CLUSTERS][MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_cluster_common_count[NUM_CLUSTERS];

/* System-wide: blocks free in ALL cores of ALL clusters */
__attribute__((section(".hbm")))
daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
volatile uint32_t g_hbm_system_common_count;

/* Locks (HBM) */
__attribute__((section(".hbm")))
volatile int g_cluster_lock[NUM_CLUSTERS];

__attribute__((section(".hbm")))
volatile int g_global_lock;

/* ------------------------------------------------------------
 *                        Tiny utilities
 * ------------------------------------------------------------ */
static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n) {
  uint32_t i; for (i = 0; i < n; ++i) p[i] = 0u;
}
static inline void daml_zero_bytes(volatile void *ptr, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t *)ptr;
  uint32_t i; for (i = 0; i < n; ++i) q[i] = 0u;
}
static inline void daml_fence(void) { __sync_synchronize(); }

static inline int daml_block_eq(const daml_block_t *a, const daml_block_t *b) {
  return (a->addr == b->addr) && (a->size == b->size);
}
static int daml_contains(const daml_block_t *list, uint32_t n, const daml_block_t *x) {
  uint32_t i; for (i = 0; i < n; ++i) if (daml_block_eq(&list[i], x)) return 1;
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
 *              API: initialization (allocators + HBM)
 * ------------------------------------------------------------ */
/* Initialize per-core allocators and clear all HBM metadata. */
static inline void soc_daml_init_allocators(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER],
                                            uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
  uint32_t c, core;

  for (c = 0; c < NUM_CLUSTERS; ++c) {
    for (core = 0; core < CORES_PER_CLUSTER; ++core) {
      /* Initialize allocator (from flex_alloc.h) */
      flex_cluster_alloc_init(&g_hbm_l1_allocators[c][core],
                              base_addrs[c][core],
                              sizes[c][core]);
    }
  }

  /* Clear locks and metadata (no memset) */
  for (c = 0; c < NUM_CLUSTERS; ++c) {
    g_cluster_lock[c] = 0;
    g_hbm_cluster_common_count[c] = 0u;
    daml_zero_bytes(&g_hbm_cluster_common[c][0], (uint32_t)sizeof(g_hbm_cluster_common[c]));
    for (core = 0; core < CORES_PER_CLUSTER; ++core) {
      g_hbm_core_free_count[c][core] = 0u;
      daml_zero_bytes(&g_hbm_core_free[c][core][0], (uint32_t)sizeof(g_hbm_core_free[c][core]));
    }
  }
  g_global_lock = 0;
  g_hbm_system_common_count = 0u;
  daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
  daml_fence(); /* publish zeros */
}

/* ------------------------------------------------------------
 *              API: per-core snapshot upload (P1)
 * ------------------------------------------------------------ */
/*
 * Walk the local allocator free-list of (cluster_id, core_id) and
 * publish (addr,size) pairs into HBM.
 * Requires: g_hbm_l1_allocators[...] reflects the allocator used.
 */
static inline void soc_daml_upload_free_list(int cluster_id, int core_id) {
  alloc_t       *alloc = &g_hbm_l1_allocators[cluster_id][core_id];
  alloc_block_t *curr  = alloc->first_block;
  uint32_t count = 0u;

  daml_lock_cluster(cluster_id);

  /* reset count first; readers will see 0 until table is ready */
  g_hbm_core_free_count[cluster_id][core_id] = 0u;
  daml_fence();

  while (curr && (count < MAX_FREE_BLOCKS_PER_CORE)) {
    g_hbm_core_free[cluster_id][core_id][count].addr = (void*)curr;
    g_hbm_core_free[cluster_id][core_id][count].size = curr->size;
    curr  = curr->next;
    count += 1u;
  }

  /* publish table contents before making count visible */
  daml_fence();
  g_hbm_core_free_count[cluster_id][core_id] = count;

  daml_unlock_cluster(cluster_id);
}

/* ------------------------------------------------------------
 *           API: cluster-wide intersection builder (P1)
 * ------------------------------------------------------------ */
/* Find blocks free in ALL cores within cluster_id using the per-core HBM snapshots. */
static inline void soc_daml_build_cluster_common(int cluster_id) {
  uint32_t out_cnt = 0u;

  daml_lock_cluster(cluster_id);

  g_hbm_cluster_common_count[cluster_id] = 0u;
  daml_fence();

  /* Use core 0’s snapshot as reference */
  const daml_block_t *ref   = &g_hbm_core_free[cluster_id][0][0];
  const uint32_t      ref_n = g_hbm_core_free_count[cluster_id][0];
  uint32_t i, core;

  for (i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (core = 1; core < CORES_PER_CLUSTER; ++core) {
      const daml_block_t *lst   = &g_hbm_core_free[cluster_id][core][0];
      const uint32_t      lst_n = g_hbm_core_free_count[cluster_id][core];
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
 *          API: system-wide intersection builder (opt)
 * ------------------------------------------------------------ */
/* Find blocks free in ALL cores of ALL clusters (uses cluster_common as inputs). */
static inline void soc_daml_build_system_common(void) {
  uint32_t out_cnt = 0u, c, i;

  daml_lock_global();

  g_hbm_system_common_count = 0u;
  daml_fence();

  if (NUM_CLUSTERS == 0) { daml_unlock_global(); return; }

  const daml_block_t *ref   = &g_hbm_cluster_common[0][0];
  const uint32_t      ref_n = g_hbm_cluster_common_count[0];

  for (i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (c = 1; c < NUM_CLUSTERS; ++c) {
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
 *         Optional: wrappers around alloc/free (L1)
 * ------------------------------------------------------------ */
/*
 * These wrappers serialize allocator mutations with the cluster lock,
 * then trigger a per-core snapshot upload. The upload is done *after*
 * releasing the lock to avoid nested locking (deadlock-free).
 * You can skip these wrappers if your code calls domain_malloc/free
 * in other places — but then ensure you invoke soc_daml_upload_free_list()
 * yourself after each mutation and before intersections.
 */

static inline void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size) {
  void *ptr;
  daml_lock_cluster(cluster_id);
  ptr = domain_malloc(&g_hbm_l1_allocators[cluster_id][core_id], size);
  daml_fence();
  daml_unlock_cluster(cluster_id);

  /* publish fresh snapshot */
  soc_daml_upload_free_list(cluster_id, core_id);
  return ptr;
}

static inline void flex_l1_block_free(int cluster_id, int core_id, void *ptr) {
  daml_lock_cluster(cluster_id);
  domain_free(&g_hbm_l1_allocators[cluster_id][core_id], ptr);
  daml_fence();
  daml_unlock_cluster(cluster_id);

  /* publish fresh snapshot */
  soc_daml_upload_free_list(cluster_id, core_id);
}

/* ------------------------------------------------------------
 *                       Convenience getters
 * ------------------------------------------------------------ */
static inline uint32_t soc_daml_get_core_free_count(int cluster_id, int core_id) {
  return g_hbm_core_free_count[cluster_id][core_id];
}
static inline const daml_block_t* soc_daml_get_core_free_list(int cluster_id, int core_id) {
  return &g_hbm_core_free[cluster_id][core_id][0];
}

static inline uint32_t soc_daml_get_cluster_common_count(int cluster_id) {
  return g_hbm_cluster_common_count[cluster_id];
}
static inline const daml_block_t* soc_daml_get_cluster_common(int cluster_id) {
  return &g_hbm_cluster_common[cluster_id][0];
}

static inline uint32_t soc_daml_get_system_common_count(void) {
  return g_hbm_system_common_count;
}
static inline const daml_block_t* soc_daml_get_system_common(void) {
  return &g_hbm_system_common[0];
}
