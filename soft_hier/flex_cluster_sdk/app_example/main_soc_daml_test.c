#include "flex_runtime.h"
#include "flex_printf.h"       // platform tiny printf
#include "flex_cluster_arch.h"
#include "flex_alloc.h"
#include "soc_daml.h"
#include <stdint.h>

static inline uint32_t as_u32(const void *p) { return (uint32_t)(uintptr_t)p; }

int main(void)
{
  uint32_t eoc_val = 0;

  flex_barrier_xy_init();
  flex_global_barrier_xy();

  const uint32_t mesh_x   = ARCH_NUM_CLUSTER_X;
  const uint32_t mesh_y   = ARCH_NUM_CLUSTER_Y;
  const uint32_t clusters = mesh_x * mesh_y;
  const uint32_t cores    = ARCH_NUM_CORE_PER_CLUSTER;

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("[BOOT] Using %ux%u clusters, %u cores/cluster\n", mesh_x, mesh_y, cores);
    printf("[BOOT] Building allocator maps...\n");
    printf("[BOOT] Initializing allocators in HBM...\n");
    soc_daml_reset_metadata();                 // (A) global HBM metadata only
  }
  flex_global_barrier_xy();

  /* (B) Each core initializes its own local L1 arena */
  const uint32_t my_cid  = flex_get_cluster_id();
  const uint32_t my_core = flex_get_core_id();

  /* Derive a local arena inside cluster TCDM */
  const uint32_t tcdm_base      = ARCH_CLUSTER_TCDM_BASE;
  const uint32_t tcdm_size      = ARCH_CLUSTER_TCDM_SIZE;

  /* Some SDKs generate heap base/end; fall back to 0x100..end if not defined */
  const uint32_t heap_base_off  =
#ifdef ARCH_CLUSTER_HEAP_BASE
    ARCH_CLUSTER_HEAP_BASE;
#else
    0x00000100u;
#endif

  const uint32_t heap_base      = tcdm_base + heap_base_off;
  const uint32_t heap_bytes     = tcdm_size - heap_base_off;

  const uint32_t per_core_bytes = (heap_bytes / cores) & ~((uint32_t)sizeof(alloc_block_t) - 1u);
  const uint32_t my_core_off    = heap_base_off + my_core * per_core_bytes;
  const uint32_t my_core_size   = per_core_bytes;

  void *my_local_base = (void*)(uintptr_t)(tcdm_base + my_core_off);

  /* IMPORTANT: run this on the core owning that L1 */
  soc_daml_init_local_allocator((int)my_cid, (int)my_core, my_local_base, my_core_size);

  /* Publish my free-list snapshot */
  soc_daml_upload_free_list((int)my_cid, (int)my_core);

  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 1: Per-core snapshots ===\n");
    for (uint32_t c = 0; c < clusters; ++c)
      for (uint32_t k = 0; k < cores; ++k)
        print_core_snapshot((int)c,(int)k);
  }
  flex_global_barrier_xy();

  if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
    printf("\n=== Phase 2: Cluster-wide intersections ===\n");
    for (uint32_t c = 0; c < clusters; ++c) {
      soc_daml_build_cluster_common((int)c);
      print_cluster_common((int)c);
    }
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

  /* ------------------- HBM via iDMA tutorial flow ------------------- */
  if (flex_is_dm_core() && (flex_get_cluster_id() == 0)) {
    printf("\n=== HBM <-> L1 via iDMA (tutorial pattern) ===\n");

    // Suppose A is preloaded at HBM offset 0 (see make_preload_elf)
    uint64_t A_hbm_offset   = 0;                 // HBM base + 0
    uint32_t A_l1_offset    = 0;                 // start of local L1 buffer
    uint32_t A_size_bytes   = 64 * 64 * 2;       // 64x64 fp16

    volatile uint16_t* l1p = (volatile uint16_t*)local(A_l1_offset);

    printf("[Before HBM->L1] first 8 L1 words:\n");
    for (int i = 0; i < 8; ++i) printf("  0x%04x\n", l1p[i]);

    // HBM -> L1
    flex_dma_async_1d(local(A_l1_offset),
                      hbm_addr(A_hbm_offset),
                      A_size_bytes);
    printf("[Load HBM->L1] async iDMA...\n");
    flex_dma_async_wait_all();

    printf("[After  HBM->L1] first 8 L1 words:\n");
    for (int i = 0; i < 8; ++i) printf("  0x%04x\n", l1p[i]);

    // Store back L1 -> HBM to a new region (e.g., 4 * A_size_bytes)
    uint64_t A_copy_hbm_offset = 4ull * (uint64_t)A_size_bytes;
    flex_dma_async_1d(hbm_addr(A_copy_hbm_offset),
                      local(A_l1_offset),
                      A_size_bytes);
    printf("[Store L1->HBM] async iDMA...\n");
    flex_dma_async_wait_all();

    // Optional: dump both regions to a file
    printf("[Dump HBM regions]\n");
    flex_dump_open();
    flex_dump_hbm(A_hbm_offset,       A_size_bytes);
    flex_dump_hbm(A_copy_hbm_offset,  A_size_bytes);
    flex_dump_close();
  }
  flex_global_barrier_xy();
  /* ------------------------------------------------------------------ */

  flex_eoc(eoc_val);
  return 0;
}
