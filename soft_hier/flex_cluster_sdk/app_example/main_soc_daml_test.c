#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"

/* Ordered printing so logs are readable */
static void print_in_cluster_core_order(const char *tag)
{
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("%s\n", tag);
    }
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

    /* Boot sync & timer */
    flex_barrier_xy_init();
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        flex_timer_start();
        /* clear top-level system-common outputs */
        g_hbm_system_common_count = 0u;
        daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
    }
    flex_global_barrier_xy();

    soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);

    /* Per-cluster allocator init on owner */
    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        flex_global_barrier_xy();
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
            printf("[BOOT] Init allocators (cluster %u)\n", cid);
            soc_daml_init_allocators_this_cluster(cid);
            printf("[BOOT] Done (cluster %u)\n", cid);
        }
        flex_global_barrier_xy();
    }

    /* Snapshots: each (cid,kid) does its own */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[BOOT] Snapshot begin C%u/K%u\n", cid, kid);
            }
            flex_global_barrier_xy();

            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                soc_daml_upload_free_list(cid, kid);
            }

            flex_global_barrier_xy();
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[BOOT] Snapshot end   C%u/K%u\n", cid, kid);
            }
            flex_global_barrier_xy();
        }
    }

    /* Build per-core bitmaps, then cluster-free (OR), then system-common (AND) */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building per-core FREE bitmaps (S=0x%08x)...\n", (unsigned)SLOT_SIZE);
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                soc_daml_build_core_bitmap(cid, kid);
            }
            flex_global_barrier_xy();
        }
    }

    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building CLUSTER-FREE bitmaps (OR across cores)...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        flex_global_barrier_xy();
        if (flex_get_cluster_id() == cid && flex_get_core_id() == 0) {
            soc_daml_build_cluster_free_bitmap(cid);
        }
        flex_global_barrier_xy();
    }

    /* System = AND across clusters + compress to ranges */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building SYSTEM-COMMON (AND across clusters)...\n");
        soc_daml_build_system_common_bitmap();
        soc_daml_system_common_bitmap_to_ranges();
        uint32_t n = soc_daml_get_system_common_count();
        printf("[BOOT] System common ranges = %u\n", n);
        const daml_block_t *lst = soc_daml_get_system_common();
        /* print up to a handful */
        uint32_t to_show = (n > 8u) ? 8u : n;
        for (uint32_t i = 0; i < to_show; ++i) {
            uint32_t lo = (uint32_t)(uintptr_t)lst[i].addr;
            uint32_t hi = lo + lst[i].size;
            printf("    [COMMON %u] [0x%08x .. 0x%08x) size=0x%08x\n", i, lo, hi, lst[i].size);
        }
    }
    flex_global_barrier_xy();

    /* Optional: ordered hello */
    print_in_cluster_core_order("HELLO-ORDERED");

    /* Done */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        flex_timer_end();
    }
    flex_global_barrier_xy();
    flex_eoc(eoc_val);
    return 0;
}
