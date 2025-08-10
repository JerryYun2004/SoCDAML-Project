#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_dma_pattern.h"

#include "soc_daml.h"   // our header above

/* Print once per cluster, in order, gated by barriers */
static void ordered_print_all_clusters(const char *msg)
{
    flex_global_barrier_xy();
    for (int cid = 0; cid < ARCH_NUM_CLUSTER; ++cid)
    {
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid)
        {
            printf("[Cluster %2d] %s\n", cid, msg);
        }
        flex_global_barrier_xy();
    }
    flex_global_barrier_xy();
}

/* Print per core in order: for each cluster -> for each core */
static void ordered_print_all_cores(const char *msg)
{
    flex_global_barrier_xy();
    for (int cid = 0; cid < ARCH_NUM_CLUSTER; ++cid)
    {
        for (int kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid)
        {
            if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid)
            {
                if (kid == 0) printf("[Cluster %2d] -----\n", cid);
            }
            if (flex_get_core_id() == kid && flex_get_cluster_id() == cid)
            {
                printf("    [C%2d/K%2d] %s\n", cid, kid, msg);
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();
}

int main(void)
{
    uint32_t eoc_val = 0;

    flex_barrier_xy_init();
    flex_global_barrier_xy();

    /* Basic system banner */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("[BOOT] Building allocator maps...\n");
    }
    flex_global_barrier_xy();

    /* Publish runtime dims to the metadata runtime */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);
        /* zeroed in init function, but this line keeps the print order readable */
        printf("[BOOT] Global metadata zeroed.\n");
    }
    flex_global_barrier_xy();

    /* Initialize per-core allocators (HBM-visible metadata pointing into L1 arenas) */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        printf("[BOOT] Initializing allocators in HBM...\n");
    }
    flex_global_barrier_xy();

    /* Do the actual init once: core0 of cluster0 */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        soc_daml_init_allocators();
    }
    flex_global_barrier_xy();

    /* Upload per-core free list snapshots in strict (cluster, core) order with tiny debug prints */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    for (int cid = 0; cid < ARCH_NUM_CLUSTER; ++cid)
    {
        for (int kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid)
        {
            if (flex_get_core_id() == kid && flex_get_cluster_id() == cid)
            {
                printf("[BOOT] Snapshot begin C%u/K%u\n", cid, kid);
                soc_daml_upload_free_list(cid, kid);
                printf("[BOOT] Snapshot end   C%u/K%u\n", cid, kid);
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();

    /* Build cluster-wide intersections (one cluster at a time) */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        printf("[BOOT] Building cluster-wide intersections...\n");
    }
    flex_global_barrier_xy();

    for (int cid = 0; cid < ARCH_NUM_CLUSTER; ++cid)
    {
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid)
        {
            printf("[BOOT] Intersect begin Cluster %u\n", cid);
            soc_daml_build_cluster_common(cid);
            printf("[BOOT] Intersect done  Cluster %u\n", cid);
        }
        flex_global_barrier_xy();
    }
    flex_global_barrier_xy();

    /* Build system-wide intersection (once) */
    if (flex_get_core_id() == 0 && flex_get_cluster_id() == 0)
    {
        printf("[BOOT] Building system-wide intersection...\n");
        soc_daml_build_system_common();
        printf("[BOOT] System-wide intersection done.\n");
    }
    flex_global_barrier_xy();

    /* Ordered prints to demonstrate clean, serialized output */
    ordered_print_all_clusters("Hello from cluster lead");
    ordered_print_all_cores("Hello from core");

    /* Done */
    flex_global_barrier_xy();
    flex_eoc(eoc_val);
    return 0;
}
