/* Reduce tiny-printf feature set to shrink .rodata/.text */
#define PRINTF_DISABLE_SUPPORT_FLOAT
#define PRINTF_DISABLE_SUPPORT_LONG_LONG
#include "flex_printf.h"       /* provides printf() via macro */

#include "flex_runtime.h"      /* barriers, IDs, EOC */
#include "flex_cluster_arch.h" /* ARCH_NUM_* macros (generated after `make hw`) */
#include "flex_alloc.h"        /* flex_hbm_malloc/free */
#include "soc_daml.h"          /* P1 APIs */
#include <stdint.h>

/* Limit L1 arena size to keep demo tight */
#define L1_MEM_SIZE 2048u

/* Smaller, fixed-capacity arenas: [clusters][cores][bytes] */
static uint8_t l1_mem[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                     [ARCH_NUM_CORE_PER_CLUSTER]
                     [L1_MEM_SIZE];

static inline uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

/* Build base/size maps without libc */
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
  printf("[C%u K%u] free_cnt=%u\n", (unsigned)c, (unsigned)k, (unsigned)n);

  /* Print at most a couple of entries to limit format strings */
  if (n) {
    const daml_block_t *b0 = &soc_daml_get_core_free_list(c, k)[0];
    printf("  #0 A=0x%08x S=%u\n", as_u32(b0->addr), (unsigned)b0->size);
    if (n > 1) {
      const daml_block_t *b1 = &soc_daml_get_core_free_list(c, k)[1];
      printf("  #1 A=0x%08x S=%u%s\n",
             as_u32(b1->addr), (unsigned)b1->size, (n>2) ? " ..." : "");
    }
  }
}

static void print_cluster_common(int c)
{
  uint32_t n = soc_daml_get_cluster_common_count(c);
  printf("[C%u] common_cnt=%u\n", (unsigned)c, (unsigned)n);
  if (n) {
    const daml_block_t *b0 = &soc_daml_get_cluster_common(c)[0];
    printf("  #0 A=0x%08x S=%u%s\n",
           as_u32(b0->addr), (unsigned)b0->size, (n>1) ? " ..." : "");
  }
}

int main(void)
{
  uint32_t eoc_val = 0;

  flex_barrier_xy_init();
  flex_global_barrier_xy();

  const uint32_t mesh_x   = ARCH_NUM_CLUSTER_X;
  const uint32_t mesh_y   = ARCH_NUM_CLUSTER_Y;
  const uint32_t clusters = mesh_x * mesh_y;
  const uint32_t cores    = ARCH_NUM_CORE_PER_CLUSTER;

  /* Single line header to reduce .rodata */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] %ux%u clusters, %u cores/cluster\n", mesh_x, mesh_y, cores);
  }

  soc_daml_set_runtime_dims(clusters, cores);

  /* Static maps to avoid dynamic allocations */
  static void *base_addrs[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)][ARCH_NUM_CORE_PER_CLUSTER];
  static uint32_t sizes[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)][ARCH_NUM_CORE_PER_CLUSTER];

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] init allocators...\n");
    build_maps(base_addrs, sizes, clusters, cores);
    soc_daml_init_allocators(base_addrs, sizes);
  }
  flex_global_barrier_xy();

  /* Each core uploads its snapshot */
  const int my_cid  = (int)flex_get_cluster_id();
  const int my_core = (int)flex_get_core_id();
  if (((unsigned)my_cid < clusters) && ((unsigned)my_core < cores)) {
    soc_daml_upload_free_list(my_cid, my_core);
  }
  flex_global_barrier_xy();

  /* Core0/Cluster0 prints a compact view */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[P1] snapshots\n");
    for (uint32_t c = 0; c < clusters; ++c)
      for (uint32_t k = 0; k < cores; ++k)
        print_core_snapshot((int)c,(int)k);
  }
  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[P1] cluster intersections\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* Mutate a little in C0/K0 */
  if (my_cid == 0 && my_core == 0) {
    void *p0 = flex_l1_block_alloc(0,0,128u);
    void *p1 = flex_l1_block_alloc(0,0,96u);
    printf("[C0 K0] alloc p0=0x%08x p1=0x%08x\n", as_u32(p0), as_u32(p1));
    if (p0) flex_l1_block_free(0,0,p0);
  }
  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[P1] intersections after mutate\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* HBM quick test (short prints) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    void *h = flex_hbm_malloc(64u);
    printf("[HBM] 64B @0x%08x\n", as_u32(h));
    if (h) {
      volatile uint8_t *q = (volatile uint8_t*)h;
      for (uint32_t i = 0; i < 64u; ++i) q[i] = (uint8_t)i;
      uint32_t ok = 1u;
      for (uint32_t i = 0; i < 64u; ++i) if (q[i] != (uint8_t)i) { ok = 0u; break; }
      printf("[HBM] verify=%u\n", (unsigned)ok);
      flex_hbm_free((void*)h);
    }
  }

  flex_global_barrier_xy();
  flex_eoc(eoc_val);
  return 0;
}
