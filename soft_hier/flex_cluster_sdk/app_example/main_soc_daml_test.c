#include "flex_runtime.h"     // barriers, IDs
#include "flex_printf.h"      // lightweight printf
#include "flex_cluster_arch.h"// ARCH_NUM_* macros (generated after `make hw`)
#include "flex_alloc.h"       // flex_hbm_malloc/free
#include "soc_daml.h"         // P1 APIs + derived sizes
#include <stdint.h>

/* Small per-(cluster,core) arenas for L1 allocator testing. */
#define L1_MEM_SIZE 4096u
static uint8_t l1_mem[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                     [ARCH_NUM_CORE_PER_CLUSTER]
                     [L1_MEM_SIZE];

static inline uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

static void build_maps(void *base[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                              [ARCH_NUM_CORE_PER_CLUSTER],
                       uint32_t size[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                                    [ARCH_NUM_CORE_PER_CLUSTER],
                       uint32_t clusters, uint32_t cores)
{
  for (uint32_t c = 0; c < clusters; ++c) {
    for (uint32_t k = 0; k < cores; ++k) {
      base[c][k] = (void*)&l1_mem[c][k][0];
      size[c][k] = L1_MEM_SIZE;
    }
  }
}

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

  flex_barrier_xy_init();
  flex_global_barrier_xy();

  /**************************************/
  /*  Program Execution Region -- Start */
  /**************************************/

  const uint32_t mesh_x   = ARCH_NUM_CLUSTER_X;
  const uint32_t mesh_y   = ARCH_NUM_CLUSTER_Y;
  const uint32_t clusters = mesh_x * mesh_y;                /* e.g., 16 */
  const uint32_t cores    = ARCH_NUM_CORE_PER_CLUSTER;      /* e.g., 3  */

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] Using %ux%u clusters, %u cores/cluster\n", mesh_x, mesh_y, cores);
  }

  /* Let soc_daml know the exact runtime mesh size */
  soc_daml_set_runtime_dims(clusters, cores);

  /* Build allocator maps and initialize HBM mirrors once */
  static void *base_addrs[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)][ARCH_NUM_CORE_PER_CLUSTER];
  static uint32_t sizes[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)][ARCH_NUM_CORE_PER_CLUSTER];

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] Building allocator maps...\n");
    build_maps(base_addrs, sizes, clusters, cores);
    printf("[BOOT] Initializing allocators in HBM...\n");
    soc_daml_init_allocators(base_addrs, sizes);
  }
  flex_global_barrier_xy();

  /* Each core uploads its L1 free-list snapshot to HBM */
  const int my_cid  = (int)flex_get_cluster_id();
  const int my_core = (int)flex_get_core_id();
  if (((unsigned)my_cid < clusters) && ((unsigned)my_core < cores)) {
    soc_daml_upload_free_list(my_cid, my_core);
  }
  flex_global_barrier_xy();

  /* Master prints snapshots for all participants */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 1: Per-core snapshots ===\n");
    for (uint32_t c = 0; c < clusters; ++c)
      for (uint32_t k = 0; k < cores; ++k)
        print_core_snapshot((int)c,(int)k);
  }
  flex_global_barrier_xy();

  /* Build cluster-wide intersections from snapshots */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 2: Cluster-wide intersections ===\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* Mutate allocator on one core to demonstrate change + re-snapshot */
  if (my_cid == 0 && my_core == 0) {
    printf("\n[C0-K0] alloc 128, then 96; free first; re-snapshot\n");
    void *p0 = flex_l1_block_alloc(0,0,128u);   /* wrapper locks + fence + snapshot */
    void *p1 = flex_l1_block_alloc(0,0,96u);
    printf("[C0-K0] p0=0x%08x p1=0x%08x\n", as_u32(p0), as_u32(p1));
    if (p0) flex_l1_block_free(0,0,p0);         /* wrapper locks + fence + snapshot */
  }
  flex_global_barrier_xy();

  /* Rebuild intersections after mutation */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 3: Intersections after mutations ===\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* HBM sanity: alloc -> write -> read -> free */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== HBM: alloc->write->read->free ===\n");
    void *h = flex_hbm_malloc(64u);
    printf("[HBM] alloc 64 -> 0x%08x\n", as_u32(h));
    if (h) {
      volatile uint8_t *q = (volatile uint8_t*)h;
      for (uint32_t i = 0; i < 64u; ++i) q[i] = (uint8_t)i;
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
