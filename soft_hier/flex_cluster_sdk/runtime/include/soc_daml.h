#pragma once

#include <stdint.h>
#include <stddef.h>
#include "flex_alloc.h"  // uses alloc_t, alloc_block_t, flex_cluster_alloc_init(), domain_malloc(), domain_free()

/*
 * ============================================================
 *            System / Cluster Configuration (tune)
 * ============================================================
 */
#ifndef NUM_CLUSTERS
#define NUM_CLUSTERS  2
#endif

#ifndef CORES_PER_CLUSTER
#define CORES_PER_CLUSTER  8
#endif

/* Maximum number of free blocks we record per (cluster, core) snapshot.
 * Keep large enough for your workloads; intersection uses these snapshots. */
#ifndef MAX_FREE_BLOCKS_PER_CORE
#define MAX_FREE_BLOCKS_PER_CORE  128
#endif

/* Maximum number of intersection results we keep per cluster/system. */
#ifndef MAX_COMMON_BLOCKS
#define MAX_COMMON_BLOCKS  128
#endif

/*
 * ============================================================
 *                  Data Structures in HBM
 * ============================================================
 * All globally-visible metadata lives in HBM so any core in any
 * cluster can read them through the NoC (as required by P1).    [slides]
 */

typedef struct {
  void    *addr;     // L1 start address of the free block (equals block header address)
  uint32_t size;     // size in bytes of that free block
} daml_block_t;

/* 1) Per-(cluster, core) snapshots of L1 free-list blocks */
__attribute__((section(".hbm")))
static daml_block_t g_hbm_free_blocks[NUM_CLUSTERS][CORES_PER_CLUSTER][MAX_FREE_BLOCKS_PER_CORE];

__attribute__((section(".hbm")))
static volatile uint32_t g_hbm_free_block_counts[NUM_CLUSTERS][CORES_PER_CLUSTER];

/* 2) Per-cluster intersection (free in ALL cores of that cluster) */
__attribute__((section(".hbm")))
static daml_block_t g_hbm_cluster_common[NUM_CLUSTERS][MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
static volatile uint32_t g_hbm_cluster_common_count[NUM_CLUSTERS];

/* 3) System-wide intersection (free in ALL cores of ALL clusters) */
__attribute__((section(".hbm")))
static daml_block_t g_hbm_system_common[MAX_COMMON_BLOCKS];

__attribute__((section(".hbm")))
static volatile uint32_t g_hbm_system_common_count;

/* 4) (Optional) Global view of each core’s allocator (if you want to read it remotely) */
__attribute__((section(".hbm")))
static alloc_t g_hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

/*
 * ============================================================
 *                        Synchronization
 * ============================================================
 * Simple spinlocks per cluster and a global one.
 * We avoid libc; use GCC atomic builtins.
 */
__attribute__((section(".hbm")))
static volatile int g_cluster_lock[NUM_CLUSTERS];

__attribute__((section(".hbm")))
static volatile int g_global_lock;

static inline void daml_lock_cluster(int cluster_id) {
  while (__sync_lock_test_and_set(&g_cluster_lock[cluster_id], 1)) { }
}

static inline void daml_unlock_cluster(int cluster_id) {
  __sync_lock_release(&g_cluster_lock[cluster_id]);
}

static inline void daml_lock_global(void) {
  while (__sync_lock_test_and_set(&g_global_lock, 1)) { }
}

static inline void daml_unlock_global(void) {
  __sync_lock_release(&g_global_lock);
}

/*
 * ============================================================
 *                      Small Utilities (no libc)
 * ============================================================
 */

static inline void daml_zero_u32(volatile uint32_t *p, uint32_t n) {
  uint32_t i;
  for (i = 0; i < n; ++i) { p[i] = 0u; }
}

static inline void daml_zero_bytes(volatile void *p, uint32_t n) {
  volatile uint8_t *q = (volatile uint8_t *)p;
  uint32_t i;
  for (i = 0; i < n; ++i) { q[i] = 0u; }
}

/* compare (addr,size) equality */
static inline int daml_block_eq(const daml_block_t *a, const daml_block_t *b) {
  return (a->addr == b->addr) && (a->size == b->size);
}

/* Does array B contain block X? (linear scan to stay simple/portable) */
static int daml_contains_block(const daml_block_t *B, uint32_t Bn, const daml_block_t *X) {
  uint32_t i;
  for (i = 0; i < Bn; ++i) {
    if (daml_block_eq(&B[i], X)) return 1;
  }
  return 0;
}

/*
 * ============================================================
 *                Initialization of L1 Allocators
 * ============================================================
 * base_addrs[c][core]: L1 base pointer for that core
 * sizes[c][core]:      L1 region size for that core
 *
 * Also clears HBM metadata and locks without using memset().
 * Allocator semantics/structures are from flex_alloc.h.         [API ref]
 */
static inline void soc_daml_init_allocators(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER],
                                            uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
  uint32_t c, core;

  /* init per-core allocators */
  for (c = 0; c < NUM_CLUSTERS; ++c) {
    for (core = 0; core < CORES_PER_CLUSTER; ++core) {
      /* place a copy of allocator descriptors into HBM (optional) */
      flex_cluster_alloc_init(&g_hbm_l1_allocators[c][core],
                              base_addrs[c][core],
                              sizes[c][core]);                   // from flex_alloc.h
    }
  }

  /* zero metadata (no memset) */
  for (c = 0; c < NUM_CLUSTERS; ++c) {
    g_hbm_cluster_common_count[c] = 0u;
    daml_zero_bytes(&g_hbm_cluster_common[c][0], (uint32_t)sizeof(g_hbm_cluster_common[c]));

    for (core = 0; core < CORES_PER_CLUSTER; ++core) {
      g_hbm_free_block_counts[c][core] = 0u;
      daml_zero_bytes(&g_hbm_free_blocks[c][core][0],
                      (uint32_t)sizeof(g_hbm_free_blocks[c][core]));
      g_cluster_lock[c] = 0;
    }
  }
  g_global_lock = 0;
  g_hbm_system_common_count = 0u;
  daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
}

/*
 * ============================================================
 *                    Upload Free-List to HBM  (P1)
 * ============================================================
 * For the calling (cluster_id, core_id), walk its L1 allocator
 * free-list and copy (addr,size) pairs into HBM snapshot.
 *
 * IMPORTANT:
 *  - The free-list node lives AT THE START of each free block
 *    (block header located at block start)                   [flex_alloc.h]
 *  - We simply publish (addr = node pointer, size = node->size)
 */
static inline void soc_daml_upload_free_list(int cluster_id, int core_id) {
  /* In many deployments allocators live in L1; here we assume
     we have visibility through g_hbm_l1_allocators mirror. */
  alloc_t          *alloc = &g_hbm_l1_allocators[cluster_id][core_id];
  alloc_block_t    *curr  = alloc->first_block;
  uint32_t          count = 0;

  daml_lock_cluster(cluster_id);

  /* reset old snapshot */
  g_hbm_free_block_counts[cluster_id][core_id] = 0u;

  while (curr && count < MAX_FREE_BLOCKS_PER_CORE) {
    g_hbm_free_blocks[cluster_id][core_id][count].addr = (void*)curr;
    g_hbm_free_blocks[cluster_id][core_id][count].size = curr->size;
    curr  = curr->next;
    count += 1u;
  }

  g_hbm_free_block_counts[cluster_id][core_id] = count;

  daml_unlock_cluster(cluster_id);
}

/*
 * ============================================================
 *              Compute CLUSTER-wide intersection  (P1)
 * ============================================================
 * Finds blocks that appear (same address & size) in the snapshots
 * of ALL cores in the specified cluster.
 */
static inline void soc_daml_build_cluster_common(int cluster_id) {
  uint32_t core, i;
  uint32_t out_cnt = 0u;

  daml_lock_cluster(cluster_id);

  g_hbm_cluster_common_count[cluster_id] = 0u;

  /* If no cores, nothing to do */
  if (CORES_PER_CLUSTER == 0) {
    daml_unlock_cluster(cluster_id);
    return;
  }

  /* Use core 0 list as the reference */
  const daml_block_t *ref   = &g_hbm_free_blocks[cluster_id][0][0];
  const uint32_t      ref_n = g_hbm_free_block_counts[cluster_id][0];

  for (i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (core = 1; core < CORES_PER_CLUSTER; ++core) {
      const daml_block_t *lst   = &g_hbm_free_blocks[cluster_id][core][0];
      const uint32_t      lst_n = g_hbm_free_block_counts[cluster_id][core];
      if (!daml_contains_block(lst, lst_n, X)) { in_all = 0; break; }
    }

    if (in_all) {
      g_hbm_cluster_common[cluster_id][out_cnt] = *X;
      out_cnt += 1u;
    }
  }

  g_hbm_cluster_common_count[cluster_id] = out_cnt;

  daml_unlock_cluster(cluster_id);
}

/*
 * ============================================================
 *               Compute SYSTEM-wide intersection  (P1)
 * ============================================================
 * A block is system-common if it’s cluster-common in every cluster.
 * (Useful when you want the SAME L1 address across *all* clusters.)
 */
static inline void soc_daml_build_system_common(void) {
  uint32_t c, i;
  uint32_t out_cnt = 0u;

  daml_lock_global();

  g_hbm_system_common_count = 0u;

  if (NUM_CLUSTERS == 0) { daml_unlock_global(); return; }

  /* Use cluster 0 as reference */
  const daml_block_t *ref   = &g_hbm_cluster_common[0][0];
  const uint32_t      ref_n = g_hbm_cluster_common_count[0];

  for (i = 0; i < ref_n && out_cnt < MAX_COMMON_BLOCKS; ++i) {
    const daml_block_t *X = &ref[i];
    int in_all = 1;

    for (c = 1; c < NUM_CLUSTERS; ++c) {
      const daml_block_t *lst   = &g_hbm_cluster_common[c][0];
      const uint32_t      lst_n = g_hbm_cluster_common_count[c];
      if (!daml_contains_block(lst, lst_n, X)) { in_all = 0; break; }
    }

    if (in_all) {
      g_hbm_system_common[out_cnt] = *X;
      out_cnt += 1u;
    }
  }

  g_hbm_system_common_count = out_cnt;

  daml_unlock_global();
}

/*
 * ============================================================
 *            Convenience Hooks Around Allocation
 * ============================================================
 * Wrap allocation/free so we can refresh metadata for a cluster
 * after each change. (Optional but handy for demos.)
 *
 * NOTE:
 * - This uses the mirrored allocators in HBM (g_hbm_l1_allocators).
 * - If your “live” allocators reside in L1, ensure your software
 *   keeps the HBM mirrors consistent, or adapt these wrappers to
 *   point to the live allocators directly.
 */
static inline void *soc_daml_l1_alloc(int cluster_id, int core_id, uint32_t size) {
  void *p;
  daml_lock_cluster(cluster_id);
  p = domain_malloc(&g_hbm_l1_allocators[cluster_id][core_id], size);  // flex_alloc.h
  daml_unlock_cluster(cluster_id);
  /* Caller should call soc_daml_upload_free_list() afterwards */
  return p;
}

static inline void soc_daml_l1_free(int cluster_id, int core_id, void *ptr) {
  daml_lock_cluster(cluster_id);
  domain_free(&g_hbm_l1_allocators[cluster_id][core_id], ptr);         // flex_alloc.h
  daml_unlock_cluster(cluster_id);
  /* Caller should call soc_daml_upload_free_list() afterwards */
}

/*
 * ============================================================
 *                       Public Getters
 * ============================================================
 */
static inline uint32_t soc_daml_get_free_count(int cluster_id, int core_id) {
  return g_hbm_free_block_counts[cluster_id][core_id];
}

static inline const daml_block_t* soc_daml_get_free_list(int cluster_id, int core_id) {
  return &g_hbm_free_blocks[cluster_id][core_id][0];
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
