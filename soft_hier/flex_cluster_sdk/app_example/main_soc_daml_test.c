#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"

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
    }
    flex_global_barrier_xy();

    /* ---- Per-cluster allocator init (LOCAL to each cluster) ---- */
    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        /* Enter slot for cluster cid */
        flex_global_barrier_xy();
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
            printf("[BOOT] Init allocators (cluster %u)\n", cid);
            soc_daml_init_allocators_this_cluster(cid);
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
