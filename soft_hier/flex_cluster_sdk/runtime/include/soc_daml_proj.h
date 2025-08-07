// #pragma once

// #include <stddef.h>
// #include <stdint.h>
// #include <string.h>
// #include "flex_alloc.h" // Project-specific allocator header

// // ---------------------------
// // Cluster/Memory Configuration
// // ---------------------------

// // Number of clusters in the system
// #define NUM_CLUSTERS 2

// // Number of processing cores per cluster
// #define CORES_PER_CLUSTER 2

// // Max number of cluster-wide free memory blocks tracked per cluster
// #define MAX_CLUSTER_WIDE_FREE_BLOCKS 64

// // --------------------------------------------------------------
// // Per-cluster, per-core allocator state (in high-bandwidth memory)
// // --------------------------------------------------------------

// // Each core in each cluster has its own L1 allocator state, placed in HBM for bandwidth and accessibility reasons.

// // Per-core L1 allocators for each cluster
// __attribute__((section(".hbm")))
// alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

// // --------------------------------
// // Cluster-level synchronization API
// // --------------------------------

// // Acquire the spinlock for a cluster (for concurrency protection)
// void lock(int cluster_id);

// // Release the spinlock for a cluster
// void unlock(int cluster_id);

// // ------------------------------------------
// // Cluster-wide free block tracking structures
// // ------------------------------------------

// // Metadata for a block that is free (available) in all cores of a cluster, for efficient cross-core memory management.
// typedef struct {
//     void *start_addr;    // Starting address of the free memory block
//     uint32_t size;       // Size (in bytes) of the free memory block
// } free_block_info_t;

// // Cluster-wide free block records, used for tracking blocks that are free across all cores in a cluster
// __attribute__((section(".hbm")))
// free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];

// // Number of cluster-wide free blocks per cluster
// __attribute__((section(".hbm")))
// uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// // Simple spinlock array, one lock per cluster, to serialize access to shared structures
// volatile int cluster_lock[NUM_CLUSTERS] = {0};

// // -----------------------
// // Allocator Management API
// // -----------------------

// // Initialize all per-core L1 allocators for all clusters, given base addresses and memory sizes per core.
// // Also resets cluster-wide free block tracking and synchronization primitives.
// void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]);

// // Allocate a memory block of the given size from a specific core's L1 allocator.
// // Returns a pointer to the allocated memory or NULL on failure.
// void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size);

// // Free a memory block previously allocated from a specific core's L1 allocator.
// void flex_l1_block_free(int cluster_id, int core_id, void *ptr);

// // Scan all per-core allocators in a cluster and refresh the list of blocks that are free in every core (cluster-wide).
// void update_cluster_wide_free_blocks(int cluster_id);


// // merge .c files
// /*
//  * Allocator and Free Block State
//  * These arrays are placed in a special memory section (HBM = High Bandwidth Memory)
//  * to optimize memory bandwidth for allocation operations.
//  */

// /*
//  * Spinlock implementation for cluster-level synchronization.
//  * lock() acquires the lock for a given cluster (busy-wait).
//  * unlock() releases the lock.
//  */
// void lock(int cluster_id) {
//     while (__sync_lock_test_and_set(&cluster_lock[cluster_id], 1)) { /* spin until lock acquired */ }
// }

// void unlock(int cluster_id) {
//     __sync_lock_release(&cluster_lock[cluster_id]);
// }

// /*
//  * flex_l1_allocators_init
//  * Initialize every core's L1 allocator using the provided base addresses and memory sizes.
//  * Also clears out cluster-wide free block tracking structures and resets locks.
//  *
//  * Arguments:
//  *   base_addrs -- 2D array mapping [cluster][core] to their base memory addresses
//  *   sizes -- 2D array mapping [cluster][core] to their memory region sizes
//  */
// void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
//     for (int c = 0; c < NUM_CLUSTERS; ++c) {
//         for (int core = 0; core < CORES_PER_CLUSTER; ++core) {
//             // Initialize the allocator for each core in each cluster
//             flex_cluster_alloc_init(&hbm_l1_allocators[c][core], base_addrs[c][core], sizes[c][core]);
//         }
//     }
//     // Zero out cluster-wide free block tracking structures
//     memset(hbm_cluster_wide_free_blocks, 0, sizeof(hbm_cluster_wide_free_blocks));
//     memset(hbm_cluster_wide_free_block_count, 0, sizeof(hbm_cluster_wide_free_block_count));
//     memset((void*)cluster_lock, 0, sizeof(cluster_lock));
// }

// /*
//  * flex_l1_block_alloc
//  * Allocate a block of given size from a specific core's L1 allocator.
//  * Synchronizes access at the cluster level to avoid race conditions.
//  *
//  * Arguments:
//  *   cluster_id -- cluster index
//  *   core_id -- core index within the cluster
//  *   size -- size in bytes to allocate
//  * Returns:
//  *   Pointer to allocated memory or NULL on failure.
//  */
// void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size) {
//     lock(cluster_id);
//     void *ptr = domain_malloc(&hbm_l1_allocators[cluster_id][core_id], size);
//     update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after allocation
//     unlock(cluster_id);
//     return ptr;
// }

// /*
//  * flex_l1_block_free
//  * Free a previously allocated block from a specific core's L1 allocator.
//  * Synchronizes access at the cluster level to avoid race conditions.
//  *
//  * Arguments:
//  *   cluster_id -- cluster index
//  *   core_id -- core index within the cluster
//  *   ptr -- pointer to memory to free
//  */
// void flex_l1_block_free(int cluster_id, int core_id, void *ptr) {
//     lock(cluster_id);
//     domain_free(&hbm_l1_allocators[cluster_id][core_id], ptr);
//     update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after free
//     unlock(cluster_id);
// }

// /*
//  * update_cluster_wide_free_blocks
//  * Scans all cores in the given cluster to find memory blocks that are free in every core
//  * (that is, "cluster-wide" free). These blocks are recorded for efficient reuse.
//  *
//  * Algorithm:
//  *   For each block in core 0's allocator:
//  *     Check if a block at the same address/size exists and is free in all other cores.
//  *     If so, add it to the cluster-wide free block info.
//  *   Stops adding if MAX_CLUSTER_WIDE_FREE_BLOCKS is reached.
//  *
//  * Arguments:
//  *   cluster_id -- cluster index to update
//  */
// void update_cluster_wide_free_blocks(int cluster_id) {
//     hbm_cluster_wide_free_block_count[cluster_id] = 0;
//     alloc_t *core0_alloc = &hbm_l1_allocators[cluster_id][0];
//     alloc_block_t *blk0 = core0_alloc->first_block;
//     while (blk0) {
//         int is_free_all = 1;
//         // Check if this block is free in all other cores
//         for (int core = 1; core < CORES_PER_CLUSTER; ++core) {
//             alloc_t *other_alloc = &hbm_l1_allocators[cluster_id][core];
//             alloc_block_t *blk_other = other_alloc->first_block;
//             int found = 0;
//             while (blk_other) {
//                 // Compare block address and size for match
//                 if ((blk_other->size == blk0->size) && ((uintptr_t)blk_other == (uintptr_t)blk0)) {
//                     found = 1;
//                     break;
//                 }
//                 blk_other = blk_other->next;
//             }
//             if (!found) {
//                 is_free_all = 0;
//                 break;
//             }
//         }
//         // If block is free in all cores, record it for cluster-wide use
//         if (is_free_all && hbm_cluster_wide_free_block_count[cluster_id] < MAX_CLUSTER_WIDE_FREE_BLOCKS) {
//             hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].start_addr = (void*)blk0;
//             hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].size = blk0->size;
//             hbm_cluster_wide_free_block_count[cluster_id]++;
//         }
//         blk0 = blk0->next;
//     }
// }


// /*
//     Key Challenges:
//         Each cluster has its own private L1 allocator
//         Other clusters cannot access the allocator's state (stored in L1)
//         For inter-cluster DMA, the source address must be known by other clusters
//     To do:
//         Store allocated memory metadata in a globally accessible structure (e.g., in HBM)
//         Extend the domain_malloc() runtime to record allocations
//         Add logic in Cluster 1 to look up this structure to perform DMA from Cluster 0
// */

// /*
//     Structure:
//         Place it in HBM (accessible to all clusters)
//         Each allocation entry contains:
//             cluster_id
//             L1 address
//             size
//             valid flag
// */


// //Structure (Expected)
// // Define

// // Allocate memory blocks in each core to be accessible by all cores

// // Allocate memory blocks to put the commonly accessible address space

// // Computation
// // New memory use

// // Expose used memory address to commly accessible address space

// // Clustor 0 Core 0 pulls these memory addresses and compute commonly accessible ones

// // Clustor 0 Core 0 send this data to HBM

// // Intercluster check



// // Pseudo Code
// // Define a global allocation metadata structure - Add to a new header, e.g., intercluster_alloc.h:

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
