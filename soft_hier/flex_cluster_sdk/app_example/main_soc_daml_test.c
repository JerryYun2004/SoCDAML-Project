#include <stdio.h>
#include <stdint.h>
#include "soc_daml.h"

// Dummy memory regions for each core (simulate L1)
// In real hardware, these would be actual L1 memory regions.
// For simulation, we use statically allocated arrays.
#define L1_MEM_SIZE 4096
uint8_t l1_mem[NUM_CLUSTERS][CORES_PER_CLUSTER][L1_MEM_SIZE];

int main() {
    // Prepare base addresses and sizes for each core's heap
    void* base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER];
    uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER];

    for (int c = 0; c < NUM_CLUSTERS; ++c) {
        for (int core = 0; core < CORES_PER_CLUSTER; ++core) {
            base_addrs[c][core] = l1_mem[c][core];
            sizes[c][core] = L1_MEM_SIZE;
        }
    }

    printf("Initializing allocators...\n");
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

    // Free memory and update free blocks
    printf("Freeing memory and updating cluster-wide free blocks...\n");
    flex_l1_block_free(0, 0, ptr0);
    flex_l1_block_free(1, 1, ptr1);

    update_cluster_wide_free_blocks(0);
    update_cluster_wide_free_blocks(1);

    printf("Cluster-wide free blocks (cluster 0): %u\n", hbm_cluster_wide_free_block_count[0]);
    for (uint32_t i=0; i<hbm_cluster_wide_free_block_count[0]; ++i) {
        printf("  Free block %u: addr=%p size=%u\n", i,
            hbm_cluster_wide_free_blocks[0][i].start_addr,
            hbm_cluster_wide_free_blocks[0][i].size);
    }

    printf("Cluster-wide free blocks (cluster 1): %u\n", hbm_cluster_wide_free_block_count[1]);
    for (uint32_t i=0; i<hbm_cluster_wide_free_block_count[1]; ++i) {
        printf("  Free block %u: addr=%p size=%u\n", i,
            hbm_cluster_wide_free_blocks[1][i].start_addr,
            hbm_cluster_wide_free_blocks[1][i].size);
    }

    // Spinlock test
    printf("Testing cluster spinlock...\n");
    lock(0);
    printf("Cluster 0 locked!\n");
    unlock(0);
    printf("Cluster 0 unlocked!\n");

    printf("soc_daml test completed.\n");
    return 0;
}