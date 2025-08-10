#include <stdint.h>
#include "flex_runtime.h"
#include "flex_printf.h"        // defines printf as printf_()
#include "flex_cluster_arch.h"  // generated HW config
#include "flex_alloc.h"
#include "soc_daml.h"

/* ---- L1 arenas ----
 * Keep per-core arenas *inside* the cluster L1 window.
 * Use the generated HEAP base if available, otherwise conservative fallback.
 */
#define L1_ARENA_SIZE    4096u   /* 4 KiB per core */
#define L1_ARENA_STRIDE  4096u   /* simple 1:1 mapping */

static void build_maps(void *base[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                              [ARCH_NUM_CORE_PER_CLUSTER],
                       uint32_t size[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                                    [ARCH_NUM_CORE_PER_CLUSTER],
                       uint32_t clusters, uint32_t cores)
{
#ifdef ARCH_CLUSTER_HEAP_BASE
  const uint32_t heap_base = ARCH_CLUSTER_HEAP_BASE;
#else
  const uint32_t heap_base = 0x00000100u;
#endif
  const uint32_t tcdm_size = ARCH_CLUSTER_TCDM_SIZE;

  /* Optional: warn if someone grows 'cores' too much for this stride */
  const uint32_t needed = heap_base + cores * L1_ARENA_STRIDE;
  if (needed > tcdm_size && flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[WARN] L1 arena plan exceeds TCDM; trimming may be required\n");
  }

  for (uint32_t c = 0; c < clusters; ++c) {
    for (uint32_t k = 0; k < cores; ++k) {
      const uint32_t off  = heap_base + k * L1_ARENA_STRIDE;
      base[c][k] = (void*)(ARCH_CLUSTER_TCDM_BASE + off);
      size[c][k] = L1_ARENA_SIZE;
    }
  }
}

static void print_sys(void)
{
  if (flex_get_core_id()==0 && flex_get_cluster_id()==0) {
    printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
    printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
           ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
  }
}

int main(void)
{
  uint32_t eoc_val = 0;

  /* Global init/barrier */
  flex_barrier_xy_init();
  flex_global_barrier_xy();

  print_sys();
  flex_global_barrier_xy();

  const uint32_t clusters = ARCH_NUM_CLUSTER_X * ARCH_NUM_CLUSTER_Y;
  const uint32_t cores    = ARCH_NUM_CORE_PER_CLUSTER;
  soc_daml_set_runtime_dims(clusters, cores);

  if (flex_get_core_id()==0 && flex_get_cluster_id()==0) {
    printf("[BOOT] Building allocator maps...\n");
  }

  /* Build L1 allocator base/size tables (L1 addresses) */
  static void *base_addrs[(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                         [ARCH_NUM_CORE_PER_CLUSTER];
  static uint32_t sizes [(ARCH_NUM_CLUSTER_X*ARCH_NUM_CLUSTER_Y)]
                        [ARCH_NUM_CORE_PER_CLUSTER];
  build_maps(base_addrs, sizes, clusters, cores);

  flex_global_barrier_xy();
  if (flex_get_core_id()==0 && flex_get_cluster_id()==0) {
    printf("[BOOT] Initializing allocators in HBM...\n");
  }

  /* Initialize allocators (their state structures live in HBM) */
  soc_daml_init_allocators(base_addrs, sizes);

  /* Upload each core’s initial free list snapshot */
  flex_global_barrier_xy();
  for (uint32_t c = 0; c < clusters; ++c) {
    for (uint32_t k = 0; k < cores; ++k) {
      if (flex_get_cluster_id() == (int)c && flex_get_core_id() == (int)k) {
        soc_daml_upload_free_list(c, k);
      }
      flex_intra_cluster_sync();   /* keep tight and deterministic */
    }
    flex_global_barrier_xy();
  }

  /* Build cluster/system intersections once */
  if (flex_get_core_id()==0) {
    soc_daml_build_cluster_common(flex_get_cluster_id());
  }
  flex_global_barrier_xy();
  if (flex_get_cluster_id()==0 && flex_get_core_id()==0) {
    soc_daml_build_system_common();
  }
  flex_global_barrier_xy();

  if (my_cid == 0 && my_core == 0) {
    printf("\n[C0-K0] alloc 128, then 96; free first; re-snapshot\n");
    void *p0 = flex_l1_block_alloc(0,0,128u);
    void *p1 = flex_l1_block_alloc(0,0,96u);
    printf("[C0-K0] p0=0x%08x p1=0x%08x\n", as_u32(p0), as_u32(p1));
    if (p0) flex_l1_block_free(0,0,p0);
  }
  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 3: Intersections after mutations ===\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
  }
  flex_global_barrier_xy();

  /* Simple HBM sanity check using the allocator (not DMA) */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== HBM: alloc->write->read->free ===\n");
    void *h = flex_hbm_malloc(64u);
    printf("[HBM] alloc 64 -> 0x%08x\n", as_u32(h));
    if (h) {
      volatile uint8_t *q = (volatile uint8_t*)h;
      for (uint32_t i = 0; i < 64u; ++i) q[i] = (uint8_t)i;
      uint32_t ok = 1u;
      for (uint32_t i = 0; i < 64u; ++i) if (q[i] != (uint8_t)i) { ok = 0u; break; }
      printf("[HBM] verify=%s\n", ok ? "OK" : "FAIL");
      flex_hbm_free((void*)h);
    }
  }

  flex_global_barrier_xy();
  flex_eoc(eoc_val);
  return 0;
}
