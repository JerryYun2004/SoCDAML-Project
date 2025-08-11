#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"
#include "flex_alloc.h"   /* for flex_cluster_alloc_init(), alloc_t */

/*
 * DEMO to force non-zero strict intersection:
 * After soc_daml_init_allocators_this_cluster(cid) finishes, we re-seed each
 * core’s allocator in that cluster to the SAME small L1 slice. That makes
 * every (addr,size) identical, so both cluster-common and system-common > 0.
 *
 * This is only to demonstrate correctness of the runtime/intersection.
 * We do NOT allocate from these arenas in the test.
 */
#ifndef DEMO_COMMON_OVERLAP
#define DEMO_COMMON_OVERLAP 1
#endif

/* Tiny common slice inside L1 heap; adjust if you like */
#define COMMON_OFFSET  0x2000u
#define COMMON_SIZE    0x1000u  /* 4KB */

/* Ordered printing: one (cluster,core) at a time */
static void print_in_cluster_core_order(const char *tag)
{
    flex_global_barrier_xy();
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
        printf("%s\n", tag);
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                printf("    [%s] C%u/K%u says hi\n", tag, cid, kid);
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();
}

int main(void)
{
    uint32_t eoc_val = 0;

    /* Global boot sync & timer */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        flex_timer_start();
    }
    flex_global_barrier_xy();

    /* Configure runtime dims for the metadata helpers */
    soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);

    /* Zero global HBM metadata once (cluster0/core0) */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building allocator maps...\n");
        /* zero locks and global/system tables */
        daml_zero_u32((volatile uint32_t*)g_cluster_lock, NUM_CLUSTERS);
        g_global_lock = 0;
        g_hbm_system_common_count = 0u;
        daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
        printf("[BOOT] Global metadata zeroed.\n");
        printf("[BOOT] Initializing allocators in HBM...\n");
#if DEMO_COMMON_OVERLAP
        printf("[DEMO] STRICT intersection demo enabled: reseeding allocators to a common L1 slice\n");
        printf("[DEMO] Target slice: base=0x%08x size=0x%08x (no allocations will use it)\n",
               (unsigned)(ARCH_CLUSTER_HEAP_BASE + COMMON_OFFSET), (unsigned)COMMON_SIZE);
#endif
    }
    flex_global_barrier_xy();

    /* ---- Per-cluster allocator init (LOCAL to each cluster) ---- */
    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        /* Enter slot for cluster cid */
        flex_global_barrier_xy();
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
            printf("[BOOT] Init allocators (cluster %u)\n", cid);
            soc_daml_init_allocators_this_cluster(cid);

#if DEMO_COMMON_OVERLAP
            /* Re-seed each core's allocator to the SAME small slice in L1.
             * Align base to alloc_block_t to respect allocator’s alignment.
             */
            uint32_t base_raw  = ARCH_CLUSTER_HEAP_BASE + COMMON_OFFSET;
            uint32_t align     = (uint32_t)sizeof(alloc_block_t);
            uint32_t common_base = (base_raw + align - 1u) & ~(align - 1u);
            uint32_t common_end  = common_base + COMMON_SIZE;
            if (common_end > ARCH_CLUSTER_HEAP_END) {
                /* Clamp if someone shrunk the heap; keep it simple. */
                common_end  = ARCH_CLUSTER_HEAP_END;
                if (common_end > common_base) {
                    /* keep at least one block worth of space */
                    uint32_t min_sz = (uint32_t)sizeof(alloc_block_t);
                    if ((common_end - common_base) < min_sz) {
                        common_base = ARCH_CLUSTER_HEAP_END - min_sz;
                    }
                }
            }

            for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
                flex_cluster_alloc_init(&g_hbm_l1_allocators[cid][kid],
                                        (void*)(uintptr_t)common_base,
                                        (uint32_t)(common_end - common_base));
            }
            printf("[DEMO] C%u reseeded: [0x%08x .. 0x%08x) size=0x%08x\n",
                   cid, common_base, common_end, (uint32_t)(common_end - common_base));
#endif
            printf("[BOOT] Done (cluster %u)\n", cid);
        }
        flex_global_barrier_xy();
    }

    /* ---- Take snapshots: each (cid,kid) does its own ---- */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
                printf("[BOOT] Snapshot begin C%u/K%u\n", cid, kid);

            flex_global_barrier_xy();

            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                soc_daml_upload_free_list(cid, kid);
            }

            flex_global_barrier_xy();
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
                printf("[BOOT] Snapshot end   C%u/K%u\n", cid, kid);
            flex_global_barrier_xy();
        }
    }

    /* ---- Build cluster-wide & system-wide intersections ---- */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building cluster-common lists...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        flex_global_barrier_xy();
        if (flex_get_cluster_id() == cid && flex_get_core_id() == 0) {
            soc_daml_build_cluster_common(cid);
            printf("[BOOT] Cluster %u common blocks = %u\n",
                   cid, soc_daml_get_cluster_common_count(cid));
        }
        flex_global_barrier_xy();
    }

    /* System-wide */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building system-common list...\n");
        soc_daml_build_system_common();
        printf("[BOOT] System common blocks = %u\n", soc_daml_get_system_common_count());
    }
    flex_global_barrier_xy();

    /* Optional: demonstrate ordered printing */
    print_in_cluster_core_order("HELLO-ORDERED");

    /* Finish */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        flex_timer_end();
    }
    flex_global_barrier_xy();
    flex_eoc(eoc_val);
    return 0;
}
