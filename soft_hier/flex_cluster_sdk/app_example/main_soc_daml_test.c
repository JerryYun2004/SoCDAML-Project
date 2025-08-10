#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_alloc.h"
#include "soc_daml.h"

/* ------------------------------------------------------------
 *               Simple per-core arena plan (LOCAL)
 * ------------------------------------------------------------ *
 * We carve a small arena per core within local L1. Pick an offset that is:
 *  - inside L1 (ARCH_CLUSTER_TCDM_BASE .. +SIZE)
 *  - not overlapping your .l1 sections
 * Here we choose base = 0x00001000 and stride per core to keep arenas distinct.
 */
#define L1_ARENA_BASE_OFFSET   (0x00001000u)   /* start of our test heap region */
#define L1_ARENA_STRIDE        (0x00004000u)   /* 16 KiB per core for demo     */
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

/* Small helper to print a per-core snapshot */
static void dump_core_snapshot(uint32_t cid, uint32_t kid)
{
  uint32_t n = soc_daml_get_core_free_count(cid, kid);
  const daml_block_t *lst = soc_daml_get_core_free_list(cid, kid);

  printf("[SNAPSHOT] C%u/K%u has %u free blocks\n", cid, kid, n);
  uint32_t show = (n < 4u) ? n : 4u; /* keep prints short */
  for (uint32_t i = 0; i < show; ++i) {
    printf("  #%u addr=0x%08x size=0x%08x\n",
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

  /* Phase A: banner */
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) {
    printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
    printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
    printf("[BOOT] Building allocator maps...\n");
  }
  flex_global_barrier_xy();

  /* Phase B: runtime dims once */
  if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) {
    soc_daml_set_runtime_dims((uint32_t)(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y),
                              (uint32_t)ARCH_NUM_CORE_PER_CLUSTER);
    soc_daml_zero_global_metadata_once();
    printf("[BOOT] Global metadata zeroed.\n");
  }
  flex_global_barrier_xy();

  /* Phase C: per-cluster allocator init (LOCAL only) */
  if (flex_get_core_id() == 0) {
    uint32_t my_cid = flex_get_cluster_id();
    void *base_row[CORES_PER_CLUSTER];
    uint32_t size_row[CORES_PER_CLUSTER];

    compute_local_arena_row(base_row, size_row);

    printf("[BOOT] Initializing allocators in HBM... (cluster %u)\n", my_cid);
    soc_daml_init_allocators_for_cluster(my_cid, base_row, size_row);
    printf("[BOOT] Allocators initialized (cluster %u).\n", my_cid);
  }
  flex_global_barrier_xy();

  /* Phase D: each core uploads its own snapshot */
  {
    uint32_t cid = flex_get_cluster_id();
    uint32_t kid = flex_get_core_id();
    soc_daml_upload_free_list(cid, kid);
  }
  flex_global_barrier_xy();

  /* Phase E: cluster-wide intersections (by core 0 in each cluster) */
  if (flex_get_core_id() == 0) {
    uint32_t cid = flex_get_cluster_id();
    soc_daml_build_cluster_common(cid);

    uint32_t m = soc_daml_get_cluster_common_count(cid);
    const daml_block_t *cl = soc_daml_get_cluster_common(cid);
    printf("[INTERSECT] Cluster %u common count = %u\n", cid, m);
    uint32_t show = (m < 4u) ? m : 4u;
    for (uint32_t i = 0; i < show; ++i) {
      printf("  C%u common #%u addr=0x%08x size=0x%08x\n",
             cid, i, (uint32_t)(uintptr_t)cl[i].addr, cl[i].size);
    }
  }
  flex_global_barrier_xy();

  /* Phase F: system-wide intersection (by C0/K0) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    soc_daml_build_system_common();
    uint32_t g = soc_daml_get_system_common_count();
    const daml_block_t *gl = soc_daml_get_system_common();
    printf("[INTERSECT] System-common count = %u\n", g);
    uint32_t show = (g < 8u) ? g : 8u;
    for (uint32_t i = 0; i < show; ++i) {
      printf("  SYS common #%u addr=0x%08x size=0x%08x\n",
             i, (uint32_t)(uintptr_t)gl[i].addr, gl[i].size);
    }
  }
  flex_global_barrier_xy();

  /* Phase G: optional snapshot dump (limit prints) */
  {
    uint32_t cid = flex_get_cluster_id();
    uint32_t kid = flex_get_core_id();
    if (kid == 0) { dump_core_snapshot(cid, 0); }
  }
  flex_global_barrier_xy();

  /* Done */
  flex_eoc(eoc_val);
  return 0;
}
