#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "soc_daml.h"

/* ------------------------------------------------------------
 *           Per-core local L1 arenas (simple static carve)
 * ------------------------------------------------------------ *
 * Choose an L1 region safely inside TCDM and away from your .l1 sections.
 * We give each core a private arena (equal size, contiguous, non-overlapping).
 */
#define L1_ARENA_BASE_OFFSET   (0x00001000u)   /* start of test heap region */
#define L1_ARENA_STRIDE        (0x00004000u)   /* 16 KiB per core            */
#define L1_ARENA_SIZE_PER_CORE (0x00004000u)

static void compute_local_arena_row(void *base_addrs_per_core[CORES_PER_CLUSTER],
                                    uint32_t sizes_per_core[CORES_PER_CLUSTER])
{
  for (uint32_t k = 0; k < CORES_PER_CLUSTER; ++k) {
    uint32_t off = L1_ARENA_BASE_OFFSET + (k * L1_ARENA_STRIDE);
    base_addrs_per_core[k] = (void*)local(off);
    sizes_per_core[k]      = L1_ARENA_SIZE_PER_CORE;
  }
}

/* ------------------------------------------------------------
 *              Ordered print helpers (cluster, core)
 * ------------------------------------------------------------ */
static inline void ordered_print_all_clusters_core0(const char *fmt)
{
  /* Cluster-by-cluster, only core 0 prints */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    flex_global_barrier_xy();
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == (int)cid) {
      printf("[C%u/K0] %s\n", cid, fmt);
    }
  }
  flex_global_barrier_xy();
}

static inline void ordered_print_all_cores(const char *fmt)
{
  /* For every cluster, for every core, exactly one prints at a time */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
      flex_global_barrier_xy();
      if (flex_get_cluster_id() == (int)cid && flex_get_core_id() == (int)kid) {
        printf("[C%u/K%u] %s\n", cid, kid, fmt);
      }
    }
  }
  flex_global_barrier_xy();
}

/* Small helper to print a short per-core snapshot (ordered outside) */
static void dump_core_snapshot(uint32_t cid, uint32_t kid)
{
  uint32_t n = soc_daml_get_core_free_count(cid, kid);
  const daml_block_t *lst = soc_daml_get_core_free_list(cid, kid);
  uint32_t show = (n < 2u) ? n : 2u;
  printf("  [C%u/K%u] %u blocks; showing %u\n", cid, kid, n, show);
  for (uint32_t i = 0; i < show; ++i) {
    printf("      #%u addr=0x%08x size=0x%08x\n",
           i, (uint32_t)(uintptr_t)lst[i].addr, lst[i].size);
  }
}

/* ------------------------------------------------------------
 *                          main
 * ------------------------------------------------------------ */
int main(void)
{
  uint32_t eoc_val = 0;

  /* Barriers setup as in the SDK examples */
  flex_barrier_xy_init();
  flex_global_barrier_xy();

  /* Phase A: banner (single print) */
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) {
    printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
    printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
    printf("[BOOT] Building allocator maps...\n");
  }
  flex_global_barrier_xy();

  /* Phase B: runtime dims & zero metadata once (C0/K0 only) */
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) {
    soc_daml_set_runtime_dims((uint32_t)(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y),
                              (uint32_t)ARCH_NUM_CORE_PER_CLUSTER);
    soc_daml_zero_global_metadata_once();
    printf("[BOOT] Global metadata zeroed.\n");
  }
  flex_global_barrier_xy();

  /* Phase C: initialize allocators cluster-by-cluster (only core 0 of each cluster) */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    flex_global_barrier_xy();
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == (int)cid) {
      void *base_row[CORES_PER_CLUSTER];
      uint32_t size_row[CORES_PER_CLUSTER];
      compute_local_arena_row(base_row, size_row);

      printf("[BOOT] Init allocators (cluster %u)\n", cid);
      soc_daml_init_allocators_for_cluster(cid, base_row, size_row);
      printf("[BOOT] Done (cluster %u)\n", cid);
    }
  }
  flex_global_barrier_xy();

  /* Phase D: upload free-lists, strictly one (cluster,core) at a time */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
      flex_global_barrier_xy();
      if (flex_get_cluster_id() == (int)cid && flex_get_core_id() == (int)kid) {
        soc_daml_upload_free_list(cid, kid);
        printf("[SNAPSHOT] Uploaded C%u/K%u\n", cid, kid);
      }
    }
  }
  flex_global_barrier_xy();

  /* Phase E: cluster-wide intersections (one cluster at a time, core0 prints) */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    flex_global_barrier_xy();
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == (int)cid) {
      soc_daml_build_cluster_common(cid);
      uint32_t m = soc_daml_get_cluster_common_count(cid);
      const daml_block_t *cl = soc_daml_get_cluster_common(cid);
      printf("[INTERSECT] Cluster %u common = %u\n", cid, m);
      uint32_t show = (m < 2u) ? m : 2u;
      for (uint32_t i = 0; i < show; ++i) {
        printf("    #%u addr=0x%08x size=0x%08x\n",
               i, (uint32_t)(uintptr_t)cl[i].addr, cl[i].size);
      }
    }
  }
  flex_global_barrier_xy();

  /* Phase F: system-wide intersection (single) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    soc_daml_build_system_common();
    uint32_t g = soc_daml_get_system_common_count();
    const daml_block_t *gl = soc_daml_get_system_common();
    printf("[INTERSECT] System-common = %u\n", g);
    uint32_t show = (g < 4u) ? g : 4u;
    for (uint32_t i = 0; i < show; ++i) {
      printf("    #%u addr=0x%08x size=0x%08x\n",
             i, (uint32_t)(uintptr_t)gl[i].addr, gl[i].size);
    }
  }
  flex_global_barrier_xy();

  /* Phase G: short per-core snapshot print — strictly ordered */
  for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y; ++cid) {
    for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
      flex_global_barrier_xy();
      if (flex_get_cluster_id() == (int)cid && flex_get_core_id() == (int)kid) {
        dump_core_snapshot(cid, kid);
      }
    }
  }
  flex_global_barrier_xy();

  /* Optional banners to demonstrate strict ordering */
  ordered_print_all_clusters_core0("hello from core0 — cluster ordered");
  ordered_print_all_cores("hello from all cores — cluster/core ordered");

  /* Done */
  flex_eoc(eoc_val);
  return 0;
}
