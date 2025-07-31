#pragma once

#include <stddef.h>
#include <stdint.h>
#include "flex_alloc.h" // use the project allocator

#define NUM_CLUSTERS 2
#define CORES_PER_CLUSTER 2
#define MAX_CLUSTER_WIDE_FREE_BLOCKS 64

// Use the allocator's types for per-core heaps
// Each core in each cluster gets its own allocator
__attribute__((section(".hbm")))
extern alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

// In soc_daml.h or a new header file
void lock(int cluster_id);
void unlock(int cluster_id);

void lock(int cluster_id) {
    while (__sync_lock_test_and_set(&cluster_lock[cluster_id], 1)) {
        // spin until the lock is acquired
    }
}

void unlock(int cluster_id) {
    __sync_lock_release(&cluster_lock[cluster_id]);
}

// Info for cluster-wide free blocks.
typedef struct {
    void *start_addr;
    uint32_t size;
} free_block_info_t;

// Per-cluster cluster-wide free blocks in HBM
__attribute__((section(".hbm")))
extern free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];
__attribute__((section(".hbm")))
extern uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// Lock per cluster for concurrency (simple spinlock for example)
extern volatile int cluster_lock[NUM_CLUSTERS];

// API functions
void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]);
void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size);
void flex_l1_block_free(int cluster_id, int core_id, void *ptr);
void update_cluster_wide_free_blocks(int cluster_id);










/*
    Key Challenges:
        Each cluster has its own private L1 allocator
        Other clusters cannot access the allocator's state (stored in L1)
        For inter-cluster DMA, the source address must be known by other clusters
    To do:
        Store allocated memory metadata in a globally accessible structure (e.g., in HBM)
        Extend the domain_malloc() runtime to record allocations
        Add logic in Cluster 1 to look up this structure to perform DMA from Cluster 0
*/

/*
    Structure:
        Place it in HBM (accessible to all clusters)
        Each allocation entry contains:
            cluster_id
            L1 address
            size
            valid flag
*/


//Structure (Expected)
// Define

// Allocate memory blocks in each core to be accessible by all cores

// Allocate memory blocks to put the commonly accessible address space

// Computation
// New memory use

// Expose used memory address to commly accessible address space

// Clustor 0 Core 0 pulls these memory addresses and compute commonly accessible ones

// Clustor 0 Core 0 send this data to HBM

// Intercluster check



// Pseudo Code
// Define a global allocation metadata structure - Add to a new header, e.g., intercluster_alloc.h:

// #define MAX_ALLOC_ENTRIES 32

// typedef struct {
//     uint32_t cluster_id;
//     uint32_t addr_l1;
//     uint32_t size;
//     uint8_t  valid;
// } intercluster_alloc_entry_t;

// typedef struct {
//     intercluster_alloc_entry_t entries[MAX_ALLOC_ENTRIES];
// } intercluster_alloc_table_t;

// // Put this table in HBM
// volatile intercluster_alloc_table_t *global_alloc_table = (intercluster_alloc_table_t *) 0x8XXXXXXX;  // HBM base address


// // Modify domain_malloc() or wrap it with a new function

// void *intercluster_malloc(alloc_t *alloc, uint32_t cluster_id, uint32_t size) {
//     void *ptr = domain_malloc(alloc, size);
//     if (!ptr) return NULL;

//     // Register the allocation in the global table
//     for (int i = 0; i < MAX_ALLOC_ENTRIES; i++) {
//         if (global_alloc_table->entries[i].valid == 0) {
//             global_alloc_table->entries[i].cluster_id = cluster_id;
//             global_alloc_table->entries[i].addr_l1 = (uint32_t) ptr;
//             global_alloc_table->entries[i].size = size;
//             global_alloc_table->entries[i].valid = 1;
//             break;
//         }
//     }

//     return ptr;
// }


// // On another cluster, retrieve the address for DMA:

// uint32_t get_allocated_address(uint32_t cluster_id) {
//     for (int i = 0; i < MAX_ALLOC_ENTRIES; i++) {
//         if (global_alloc_table->entries[i].valid &&
//             global_alloc_table->entries[i].cluster_id == cluster_id) {
//             return global_alloc_table->entries[i].addr_l1;
//         }
//     }
//     return 0; // not found
// }


// // Use iDMA to request data from another cluster

// uint32_t src_addr = get_allocated_address(/* cluster 0 */ 0);
// uint32_t dst_addr = flex_l1_malloc(size);  // locally allocated
// start_idma_transfer(src_addr, dst_addr, size);  // Implemented via DMA engine


// /*
//     Additional Tasks:
//         Define HBM base address	        - Use a known shared HBM address (aligned) to hold global_alloc_table
//         Add synchronization	            - Ensure memory barriers (__sync_something()) to avoid stale reads
//         Write DMA transfer logic        - Use the iDMA controller to transfer memory from cluster to cluster
//         Handle edge cases               - Handle table full, duplicate cluster entries, invalid entries cleanup
//         Document                        - Clearly describe how intercluster_malloc extends the runtime
// */
