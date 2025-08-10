#include "flex_runtime.h"   // barriers, core/cluster IDs
#include "soc_daml.h"       // NUM_CLUSTERS, CORES_PER_CLUSTER, P1 APIs
#include "flex_alloc.h"     // flex_hbm_malloc/free (used for HBM sanity)
#include <stdint.h>
#include <stdio.h>

/* Small per-(cluster,core) arenas for L1 allocator testing.
   Sizes match build-time NUM_CLUSTERS/CORES_PER_CLUSTER from soc_daml.h */
#define L1_MEM_SIZE 4096u
static uint8_t l1_mem[NUM_CLUSTERS][CORES_PER_CLUSTER][L1_MEM_SIZE];

/* Helpers (no compact libc) */
static inline uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

/* Build simple base/size maps for all (cluster, core) */
static void build_maps(void *base[NUM_CLUSTERS][CORES_PER_CLUSTER],
                       uint32_t size[NUM_CLUSTERS][CORES_PER_CLUSTER])
{
  for (uint32_t c = 0; c < NUM_CLUSTERS; ++c) {
    for (uint32_t k = 0; k < CORES_PER_CLUSTER; ++k) {
      base[c][k] = (void*)&l1_mem[c][k][0];
      size[c][k] = L1_MEM_SIZE;
    }
  }
}

/* Debug prints */
static void print_core_snapshot(int c, int k)
{
  uint32_t n = soc_daml_get_core_free_count(c, k);
  printf("[C%u-K%u] snapshot_count=%u\n", (unsigned)c, (unsigned)k, (unsigned)n);
  uint32_t lim = (n > 8u) ? 8u : n;
  for (uint32_t i = 0; i < lim; ++i) {
    const daml_block_t *b = &soc_daml_get_core_free_list(c, k)[i];
    printf("  #%u addr=0x%08x size=%u\n",
           (unsigned)i, as_u32(b->addr), (unsigned)b->size);
  }
  if (n > lim) printf("  ... (%u more)\n", (unsigned)(n - lim));
}

static void print_cluster_common(int c)
{
  uint32_t n = soc_daml_get_cluster_common_count(c);
  printf("[C%u] cluster_common=%u\n", (unsigned)c, (unsigned)n);
  uint32_t lim = (n > 8u) ? 8u : n;
  for (uint32_t i = 0; i < lim; ++i) {
    const daml_block_t *b = &soc_daml_get_cluster_common(c)[i];
    printf("  #%u addr=0x%08x size=%u\n",
           (unsigned)i, as_u32(b->addr), (unsigned)b->size);
  }
  if (n > lim) printf("  ... (%u more)\n", (unsigned)(n - lim));
}

int main(void)
{
  uint32_t eoc_val = 0;

  /* Same structure as the example main.c */
  flex_barrier_xy_init();
  flex_global_barrier_xy();

  /**************************************/
  /*  Program Execution Region -- Start */
  /**************************************/

  /* Build-time caps are taken from soc_daml.h; no external headers required. */
  const uint32_t clusters_cap = NUM_CLUSTERS;
  const uint32_t cores_cap    = CORES_PER_CLUSTER;

  /* Optional: let soc_daml know we’ll operate within its build-time caps.
     If your soc_daml.h already defaults to these, this is harmless. */
  soc_daml_set_runtime_dims(clusters_cap, cores_cap);

  /* Print a short header from core 0 of cluster 0 (style like main.c) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] soc_daml test using build-time caps: clusters=%u cores/cluster=%u\n",
           (unsigned)clusters_cap, (unsigned)cores_cap);
  }

  /* Build allocator maps and initialize mirrors once */
  static void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER];
  static uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER];
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] Building allocator maps...\n");
    build_maps(base_addrs, sizes);
    printf("[BOOT] Initializing allocators...\n");
    soc_daml_init_allocators(base_addrs, sizes);
  }
  flex_global_barrier_xy();

  /* Each core uploads its L1 free-list snapshot to HBM */
  const int my_cid  = (int)flex_get_cluster_id();
  const int my_core = (int)flex_get_core_id();
  soc_daml_upload_free_list(my_cid, my_core);
  flex_global_barrier_xy();

  /* Master prints snapshots for all in-bounds participants */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 1: Per-core snapshots ===\n");
    for (uint32_t c = 0; c < clusters_cap; ++c)
      for (uint32_t k = 0; k < cores_cap; ++k)
        print_core_snapshot((int)c, (int)k);
  }
  flex_global_barrier_xy();

  /* Build cluster-wide intersections */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 2: Cluster-wide intersections ===\n");
    for (uint32_t c = 0; c < clusters_cap; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* Mutate allocator on one core to demonstrate change + re-snapshot */
  if (my_cid == 0 && my_core == 0) {
    printf("\n[C0-K0] alloc 128, then 96; free first; re-snapshot\n");
    void *p0 = flex_l1_block_alloc(0, 0, 128u);   /* wrapper: locks + fence + snapshot */
    void *p1 = flex_l1_block_alloc(0, 0, 96u);
    printf("[C0-K0] p0=0x%08x p1=0x%08x\n", as_u32(p0), as_u32(p1));
    if (p0) flex_l1_block_free(0, 0, p0);         /* wrapper: locks + fence + snapshot */
  }
  flex_global_barrier_xy();

  /* Rebuild intersections after mutation */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 3: Intersections after mutations ===\n");
    for (uint32_t c = 0; c < clusters_cap; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* HBM sanity: alloc -> write -> read -> free (no compact calls) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== HBM: alloc->write->read->free ===\n");
    void *h = flex_hbm_malloc(64u);
    printf("[HBM] alloc 64 -> 0x%08x\n", as_u32(h));
    if (h) {
      volatile uint8_t *q = (volatile uint8_t*)h;
      /* write pattern */
      for (uint32_t i = 0; i < 64u; ++i) q[i] = (uint8_t)i;
      /* verify */
      uint32_t ok = 1u;
      for (uint32_t i = 0; i < 64u; ++i) { if (q[i] != (uint8_t)i) { ok = 0u; break; } }
      printf("[HBM] verify=%s\n", ok ? "OK" : "FAIL");
      flex_hbm_free((void*)h);
    }
  }

  /**************************************/
  /*  Program Execution Region -- Stop  */
  /**************************************/
  flex_global_barrier_xy();
  flex_eoc(eoc_val);
  return 0;
}
