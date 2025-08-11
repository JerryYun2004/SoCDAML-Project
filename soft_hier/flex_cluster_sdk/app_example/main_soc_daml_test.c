#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"

/* Ordered printing: one (cluster,core) at a time */
static void print_in_cluster_core_order(const char *tag)
{
    uint32_t cid, kid;

    flex_global_barrier_xy();
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0) {
        printf("%s\n", tag);
    }
    flex_global_barrier_xy();

    for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
        for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; kid++) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                printf("    [%s] C%u/K%u says hi\n", tag, cid, kid);
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();
}

/* Pretty-print a few ranges */
static void print_common_ranges(const char* tag, const daml_block_t* arr, uint32_t n)
{
    uint32_t i;
    printf("%s = %u\n", tag, n);
    for (i = 0; i < n && i < 6u; i++) {
        uint32_t a = (uint32_t)(uintptr_t)arr[i].addr;
        uint32_t e = a + arr[i].size;
        printf("    [%s #%u] [0x%08x .. 0x%08x) size=0x%08x\n",
               tag, i, a, e, arr[i].size);
    }
}

int main(void)
{
    uint32_t eoc_val = 0;
    uint32_t cid, kid;

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
        uint32_t c;
        printf("[BOOT] Building allocator maps...\n");
        /* zero locks and global/system tables */
        daml_zero_u32((volatile uint32_t*)g_cluster_lock, NUM_CLUSTERS);
        g_global_lock = 0;
        g_hbm_system_common_count = 0u;
        daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));
        /* bitmaps are cleared per-cluster during init */
        for (c = 0; c < NUM_CLUSTERS; c++) {
            g_hbm_cluster_common_count[c] = 0u;
        }
        printf("[BOOT] Global metadata zeroed.\n");
        printf("[BOOT] Initializing allocators in HBM...\n");
    }
    flex_global_barrier_xy();

    /* ---- Per-cluster allocator init (LOCAL to each cluster) ---- */
    for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
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

    for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
        for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; kid++) {
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

    /* ---- Build per-core bitmaps from snapshots ---- */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building per-core FREE bitmaps (S=0x%X)...\n", (unsigned)SLOT_SIZE);
    }
    flex_global_barrier_xy();

    if (flex_get_core_id() == 0) {
        /* One core per cluster can do all its local cores' bitmaps; for simplicity
           run it on core 0 of each cluster. */
        for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
            if (flex_get_cluster_id() == cid) {
                for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; kid++) {
                    soc_daml_build_core_bitmap(cid, kid);
                }
            }
            flex_global_barrier_xy();
        }
    } else {
        /* other cores just follow barriers */
        for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();

    /* ---- Build cluster-common bitmaps and emit ranges ---- */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building cluster-common (bitmap ANDs)...\n");
    }
    flex_global_barrier_xy();

    for (cid = 0; cid < ARCH_NUM_CLUSTER; cid++) {
        flex_global_barrier_xy();
        if (flex_get_cluster_id() == cid && flex_get_core_id() == 0) {
            soc_daml_build_cluster_common_bitmap(cid);
            soc_daml_emit_cluster_ranges_from_bitmap(cid);
            printf("[BOOT] Cluster %u common blocks = %u\n",
                   cid, soc_daml_get_cluster_common_count(cid));
        }
        flex_global_barrier_xy();
    }

    /* ---- Build system-common bitmap and emit ranges ---- */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building system-common (bitmap ANDs)...\n");
        soc_daml_build_system_common_bitmap();
        soc_daml_emit_system_ranges_from_bitmap();
        {
            uint32_t sys_n = soc_daml_get_system_common_count();
            const daml_block_t *sys = soc_daml_get_system_common();
            print_common_ranges("[BOOT] System common blocks", sys, sys_n);
        }
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
