#include "flex_runtime.h"
#include "flex_alloc.h"
#include "soc_daml.h"
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------
 *               Test L1 regions for each core
 * ------------------------------------------------------------
 * We build two scenarios to demonstrate intersections:
 *
 *  - Cluster 0: all cores share ONE backing buffer (no mutations),
 *               so right after init the cluster-wide intersection
 *               is NON-EMPTY (the single full-span free block).
 *               We then DO NOT allocate on cluster 0 to avoid
 *               corrupting the shared free-list owned by two allocators.
 *
 *  - Cluster 1: each core has its OWN backing buffer and performs
 *               independent allocations/frees, then uploads snapshots.
 *               This often yields an EMPTY intersection (as expected),
 *               which we print to demonstrate correctness.
 *
 * NOTE: This is purely for testing the metadata flow (P1).
 *       In real use, each core/cluster should have its own L1 region.
 * ------------------------------------------------------------ */

#ifndef L1_MEM_SIZE
#define L1_MEM_SIZE  4096u
#endif

/* Shared buffer for ALL cores in cluster 0 (to force a non-empty intersection at init) */
static uint8_t l1_mem_c0_shared[L1_MEM_SIZE];

/* Separate buffers for cluster 1 cores (change sizes by core to make lists different) */
static uint8_t l1_mem_c1_core0[L1_MEM_SIZE];
static uint8_t l1_mem_c1_core1[L1_MEM_SIZE];
/* If you have more than 2 cores per cluster, add more buffers or reuse pattern as needed. */

/* Helper: safe cast pointer to 32-bit for printing */
static uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

/* Helper: tiny loop (no memset) to fill base_addrs/sizes */
static void build_allocator_maps(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER],
                                 uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
  uint32_t c, core;
  for (c = 0; c < NUM_CLUSTERS; ++c) {
    for (core = 0; core < CORES_PER_CLUSTER; ++core) {
      /* Default: point to unique per-(c,core) region; we override some below. */
      base_addrs[c][core] = 0;
      sizes[c][core] = L1_MEM_SIZE;
    }
  }

  /* Cluster 0: force SAME base for all cores (non-empty intersection after init) */
  for (core = 0; core < CORES_PER_CLUSTER; ++core) {
    base_addrs[0][core] = (void *)l1_mem_c0_shared;
  }

  /* Cluster 1: distinct bases per core (show empty/variable intersections) */
  if (NUM_CLUSTERS > 1) {
    base_addrs[1][0] = (void *)l1_mem_c1_core0;
    if (CORES_PER_CLUSTER > 1) base_addrs[1][1] = (void *)l1_mem_c1_core1;
    /* For cores >=2, just reuse core0’s buffer pattern (still distinct from core1 if needed) */
    for (core = 2; core < CORES_PER_CLUSTER; ++core) {
      base_addrs[1][core] = (void *)l1_mem_c1_core0;
    }
  }
}

/* Pretty-print: a single core’s uploaded snapshot */
static void print_core_snapshot(int c, int core) {
  uint32_t n = soc_daml_get_core_free_count(c, core);
  printf("[C%u-K%u] snapshot count = %u\n", (unsigned)c, (unsigned)core, (unsigned)n);
  /* print a few entries (limit to avoid long logs) */
  uint32_t limit = n;
  if (limit > 8u) limit = 8u;
  for (uint32_t i = 0; i < limit; ++i) {
    const daml_block_t *blk = &soc_daml_get_core_free_list(c, core)[i];
    printf("  [C%u-K%u]  #%u  addr=0x%08x  size=%u\n",
           (unsigned)c, (unsigned)core, (unsigned)i,
           as_u32(blk->addr), (unsigned)blk->size);
  }
  if (n > limit) {
    printf("  ... (%u more)\n", (unsigned)(n - limit));
  }
}

/* Pretty-print: cluster-wide intersection */
static void print_cluster_common(int c) {
  uint32_t n = soc_daml_get_cluster_common_count(c);
  printf("[C%u] cluster-common count = %u\n", (unsigned)c, (unsigned)n);
  uint32_t limit = n;
  if (limit > 8u) limit = 8u;
  for (uint32_t i = 0; i < limit; ++i) {
    const daml_block_t *blk = &soc_daml_get_cluster_common(c)[i];
    printf("  [C%u]  #%u  addr=0x%08x  size=%u\n",
           (unsigned)c, (unsigned)i, as_u32(blk->addr), (unsigned)blk->size);
  }
  if (n > limit) {
    printf("  ... (%u more)\n", (unsigned)(n - limit));
  }
}

/* Pretty-print: system-wide intersection */
static void print_system_common(void) {
  uint32_t n = soc_daml_get_system_common_count();
  printf("[SYS] system-common count = %u\n", (unsigned)n);
  uint32_t limit = n;
  if (limit > 8u) limit = 8u;
  for (uint32_t i = 0; i < limit; ++i) {
    const daml_block_t *blk = &soc_daml_get_system_common()[i];
    printf("  [SYS] #%u  addr=0x%08x  size=%u\n",
           (unsigned)i, as_u32(blk->addr), (unsigned)blk->size);
  }
  if (n > limit) {
    printf("  ... (%u more)\n", (unsigned)(n - limit));
  }
}

int main(void)
{
  uint32_t eoc_val = 0;

  /* Standard runtime framing (match main.c style) */
  flex_barrier_xy_init();
  flex_global_barrier_xy();
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) flex_timer_start();

  /**************************************/
  /*  Program Execution Region -- Start */
  /**************************************/

  int my_cid  = (int)flex_get_cluster_id();
  int my_core = (int)flex_get_core_id();

  /* Build maps for allocators (same view on all cores) */
  static void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER];
  static uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER];
  if (my_cid == 0 && my_core == 0) {
    printf("[BOOT] Building allocator maps...\n");
    build_allocator_maps(base_addrs, sizes);
  }

  flex_global_barrier_xy();

  /* Initialize allocators ONCE (Core-0 of Cluster-0), then sync */
  if (my_cid == 0 && my_core == 0) {
    printf("[BOOT] Initializing allocators in HBM...\n");
    soc_daml_init_allocators(base_addrs, sizes);
    printf("[BOOT] Done init. Base C0 all-cores share buffer at 0x%08x; C1 cores have distinct buffers.\n",
           as_u32(base_addrs[0][0]));
  }
  flex_global_barrier_xy();

  /* Phase 1: each core uploads its own free-list snapshot */
  soc_daml_upload_free_list(my_cid, my_core);
  flex_global_barrier_xy();

  /* Master prints snapshots for ALL cores */
  if (my_cid == 0 && my_core == 0) {
    printf("\n=== Phase 1: Initial per-core snapshots (post-init) ===\n");
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
      for (int k = 0; k < CORES_PER_CLUSTER; ++k) {
        print_core_snapshot(c, k);
      }
    }
  }
  flex_global_barrier_xy();

  /* Build & print cluster-wide intersections from snapshots */
  if (my_cid == 0 && my_core == 0) {
    printf("\n=== Phase 2: Build cluster-wide intersections ===\n");
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
      printf("[C%u] Building cluster-common...\n", (unsigned)c);
      soc_daml_build_cluster_common(c);
      print_cluster_common(c);
    }
  }
  flex_global_barrier_xy();

  /* Phase 3: Mutate ONLY Cluster 1 allocators (avoid touching C0 shared buffer) */
  if (NUM_CLUSTERS > 1) {
    if (my_cid == 1) {
      /* Use wrappers that lock + fence + refresh snapshot */
      uint32_t alloc_sz_a = 128u + (uint32_t)(my_core * 64u);
      uint32_t alloc_sz_b = 96u;
      void *p0 = 0;
      void *p1 = 0;

      printf("[C%u-K%u] Allocating %u bytes...\n",
             (unsigned)my_cid, (unsigned)my_core, (unsigned)alloc_sz_a);
      p0 = flex_l1_block_alloc(my_cid, my_core, alloc_sz_a);
      printf("[C%u-K%u]   -> ptr 0x%08x\n", (unsigned)my_cid, (unsigned)my_core, as_u32(p0));

      printf("[C%u-K%u] Allocating %u bytes...\n",
             (unsigned)my_cid, (unsigned)my_core, (unsigned)alloc_sz_b);
      p1 = flex_l1_block_alloc(my_cid, my_core, alloc_sz_b);
      printf("[C%u-K%u]   -> ptr 0x%08x\n", (unsigned)my_cid, (unsigned)my_core, as_u32(p1));

      /* Free one block to change shape and re-upload snapshot */
      if (p0) {
        printf("[C%u-K%u] Freeing first block...\n", (unsigned)my_cid, (unsigned)my_core);
        flex_l1_block_free(my_cid, my_core, p0);
      }

      /* Snapshot refreshed by wrappers; print my own snapshot */
      print_core_snapshot(my_cid, my_core);
    }
  }
  flex_global_barrier_xy();

  /* Phase 4: Rebuild intersections after mutations on Cluster 1 */
  if (my_cid == 0 && my_core == 0) {
    printf("\n=== Phase 4: Rebuild intersections after C1 mutations ===\n");
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
      printf("[C%u] Rebuilding cluster-common...\n", (unsigned)c);
      soc_daml_build_cluster_common(c);
      print_cluster_common(c);
    }

    /* Optional system-wide intersection (will likely be zero) */
    printf("[SYS] Building system-common...\n");
    soc_daml_build_system_common();
    print_system_common();
  }
  flex_global_barrier_xy();

  /**************************************/
  /*  Program Execution Region -- Stop  */
  /**************************************/
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) flex_timer_end();
  flex_global_barrier_xy();
  flex_eoc(eoc_val);
  return 0;
}
