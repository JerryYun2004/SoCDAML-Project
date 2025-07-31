#include <stdio.h>
#include <stdint.h>
#include "soc_daml.h"

// Dummy memory regions for each core (simulate L1)
// In real hardware, these would be distinct L1 memory segments local to each processor core.
// For simulation and testing, we use statically allocated arrays in system RAM.
#define L1_MEM_SIZE 4096
uint8_t l1_mem[NUM_CLUSTERS][CORES_PER_CLUSTER][L1_MEM_SIZE];

int main() {
    // Arrays to hold the base addresses and sizes of each core's simulated L1 memory region.
    // These are used to initialize the memory allocators for each core.
    void* base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER];
    uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER];

    // Assign each core in each cluster its own memory region and size.
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
        for (int core = 0; core < CORES_PER_CLUSTER; ++core) {
            base_addrs[c][core] = l1_mem[c][core];      // Base address of memory region for each core
            sizes[c][core] = L1_MEM_SIZE;               // Size for each region (fixed for this test)
        }
    }

    printf("Initializing allocators...\n");
    // Initialize the per-core allocators with their respective memory regions.
    flex_l1_allocators_init(base_addrs, sizes);

    // Allocate memory on cluster 0, core 0
    printf("Allocating 128 bytes on cluster 0, core 0...\n");
    void* ptr0 = flex_l1_block_alloc(0, 0, 128);
    if (ptr0) printf("Allocation successful: %p\n", ptr0);
    else printf("Allocation failed!\n");

    // Allocate memory on cluster 1, core 1
    printf("Allocating 256 bytes on cluster 1, core 1...\n");
    void* ptr1 = flex_l1_block_alloc(1, 1, 256);
    if (ptr1) printf("Allocation successful: %p\n", ptr1);
    else printf("Allocation failed!\n");

    // Free the previously allocated memory blocks and update the cluster-wide free block tracking structures.
    printf("Freeing memory and updating cluster-wide free blocks...\n");
    flex_l1_block_free(0, 0, ptr0);
    flex_l1_block_free(1, 1, ptr1);

    // Scan all cores in each cluster to update the list of blocks that are free across the entire cluster.
    update_cluster_wide_free_blocks(0);
    update_cluster_wide_free_blocks(1);

    // Print the number and details of cluster-wide free blocks for cluster 0.
    printf("Cluster-wide free blocks (cluster 0): %u\n", hbm_cluster_wide_free_block_count[0]);
    for (uint32_t i = 0; i < hbm_cluster_wide_free_block_count[0]; ++i) {
        printf("  Free block %u: addr=%p size=%u\n", i,
            hbm_cluster_wide_free_blocks[0][i].start_addr,
            hbm_cluster_wide_free_blocks[0][i].size);
    }

    // Print the number and details of cluster-wide free blocks for cluster 1.
    printf("Cluster-wide free blocks (cluster 1): %u\n", hbm_cluster_wide_free_block_count[1]);
    for (uint32_t i = 0; i < hbm_cluster_wide_free_block_count[1]; ++i) {
        printf("  Free block %u: addr=%p size=%u\n", i,
            hbm_cluster_wide_free_blocks[1][i].start_addr,
            hbm_cluster_wide_free_blocks[1][i].size);
    }

    // Demonstrate the usage of the cluster spinlock for protecting cluster-wide data structures.
    printf("Testing cluster spinlock...\n");
    lock(0);
    printf("Cluster 0 locked!\n");
    unlock(0);
    printf("Cluster 0 unlocked!\n");

    printf("soc_daml test completed.\n");
    return 0;
}
