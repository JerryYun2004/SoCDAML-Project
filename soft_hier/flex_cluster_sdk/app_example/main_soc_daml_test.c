#include <stdint.h>
#include <stddef.h>

#include "flex_runtime.h"
#include "flex_printf.h"        // defines printf as printf_()
#include "flex_cluster_arch.h"  // generated HW config
#include "flex_alloc.h"
#include "flex_dma_pattern.h"   // DMA helpers (async 1D + wait)
#include "soc_daml.h"

/* ---- Small helpers ---- */
static inline uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

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

  /* Warn if someone grows 'cores' too much for this stride */
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

static void print_cluster_common_one(int cid)
{
  uint32_t n = soc_daml_get_cluster_common_count(cid);
  printf("[Common][C%02d] count=%u\n", cid, (unsigned)n);
  const daml_block_t *lst = soc_daml_get_cluster_common(cid);
  for (uint32_t i = 0; i < n && i < 4; ++i) {
    printf("  [%u] addr=0x%08x size=%u\n", (unsigned)i,
           as_u32(lst[i].addr), (unsigned)lst[i].size);
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
      if ((uint32_t)flex_get_cluster_id() == c && (uint32_t)flex_get_core_id() == k) {
        soc_daml_upload_free_list((int)c, (int)k);
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

  /* Small mutation on C0-K0 to exercise alloc/free + re-snapshot */
  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n[C0-K0] alloc 128, then 96; free first; re-snapshot\n");
    void *p0 = flex_l1_block_alloc(0,0,128u);
    void *p1 = flex_l1_block_alloc(0,0,96u);
    printf("[C0-K0] p0=0x%08x p1=0x%08x\n", as_u32(p0), as_u32(p1));
    if (p0) flex_l1_block_free(0,0,p0);
    soc_daml_upload_free_list(0,0);
  }
  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Intersections after mutations ===\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common_one((int)c);
    }
    printf("[System common] count=%u\n", (unsigned)soc_daml_get_system_common_count());
  }
  flex_global_barrier_xy();

  /* === Minimal HBM DMA sanity check (tutorial-consistent) ===
   * Only the DMA core of cluster 0 performs a tiny transfer.
   */
  if (flex_is_dm_core() && flex_get_cluster_id() == 0) {
    const uint32_t l1_off = 0;                 /* within local() space */
    const uint64_t hbm_off_src = 0x0;          /* HBM offset 0 */
    const uint64_t hbm_off_dst = 0x1000;       /* another HBM offset */

    printf("\n[DMA] L1 <- HBM (64B) from offset 0x%llx\n", (unsigned long long)hbm_off_src);
    flex_dma_async_1d(local(l1_off), hbm_addr(hbm_off_src), 64);
    flex_dma_async_wait_all();

    volatile uint8_t *buf = (volatile uint8_t*)local(l1_off);
    printf("[DMA] First 8 bytes in L1 after load: ");
    for (int i = 0; i < 8; ++i) printf(" %02x", (unsigned)buf[i]);
    printf("\n");

    printf("[DMA] HBM <- L1 (64B) to offset 0x%llx\n", (unsigned long long)hbm_off_dst);
    flex_dma_async_1d(hbm_addr(hbm_off_dst), local(l1_off), 64);
    flex_dma_async_wait_all();
  }

  flex_global_barrier_xy();
  flex_eoc(eoc_val);
  return 0;
}
