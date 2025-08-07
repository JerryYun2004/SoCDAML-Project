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
// __attribute__((section(".hbm")))
// alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

// // --------------------------------
// // Cluster-level synchronization API
// // --------------------------------

// // Array of simple spinlocks, one per cluster, for basic concurrency control.
// volatile int cluster_lock[NUM_CLUSTERS] = {0};

// // Acquire the spinlock for a cluster (for concurrency protection)
// static inline void lock(int cluster_id) {
//     while (__sync_lock_test_and_set(&cluster_lock[cluster_id], 1)) { /* spin until lock acquired */ }
// }

// // Release the spinlock for a cluster
// static inline void unlock(int cluster_id) {
//     __sync_lock_release(&cluster_lock[cluster_id]);
// }

// // ------------------------------------------
// // Cluster-wide free block tracking structures
// // ------------------------------------------

// // Metadata for a block that is free (available) in all cores of a cluster, for efficient cross-core memory management.
// typedef struct {
//     void *start_addr;    // Starting address of the free memory block
//     uint32_t size;       // Size (in bytes) of the free memory block
// } free_block_info_t;

// // Array of cluster-wide free block metadata per cluster, placed in HBM for global access.
// __attribute__((section(".hbm")))
// free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];

// // Number of cluster-wide free blocks currently tracked for each cluster (in HBM).
// __attribute__((section(".hbm")))
// uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// // -----------------------
// // Allocator Management API
// // -----------------------

// /*
//  * Initialize all per-core L1 allocators for all clusters, given base addresses and memory sizes per core.
//  * Also resets cluster-wide free block tracking and synchronization primitives.
//  */
// static inline void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
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
//  * Allocate a memory block of the given size from a specific core's L1 allocator.
//  * Returns a pointer to the allocated memory or NULL on failure.
//  */
// static inline void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size) {
//     lock(cluster_id);
//     void *ptr = domain_malloc(&hbm_l1_allocators[cluster_id][core_id], size);
//     update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after allocation
//     unlock(cluster_id);
//     return ptr;
// }

// /*
//  * Free a memory block previously allocated from a specific core's L1 allocator.
//  */
// static inline void flex_l1_block_free(int cluster_id, int core_id, void *ptr) {
//     lock(cluster_id);
//     domain_free(&hbm_l1_allocators[cluster_id][core_id], ptr);
//     update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after free
//     unlock(cluster_id);
// }

// /*
//  * Scan all per-core allocators in a cluster and refresh the list of blocks that are free in every core (cluster-wide).
//  */
// static inline void update_cluster_wide_free_blocks(int cluster_id) {
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
//         - Each cluster has its own private L1 allocator, isolated from others.
//         - Other clusters cannot directly access a cluster's allocator state (since it's stored in private L1 memory).
//         - For inter-cluster DMA (Direct Memory Access), the source address must be known and accessible by other clusters.
//     To do:
//         - Store allocated memory metadata in a globally accessible structure (e.g., in HBM).
//         - Extend the domain_malloc() runtime to record allocations in this global table.
//         - Add logic so a cluster can lookup this table and perform DMA transfers based on recorded allocations.
// */

// /*
//     Design for Inter-cluster Accessible Allocation Table:
//         - Place the table in HBM (so all clusters can read/write it).
//         - Each allocation record in the table contains:
//             - cluster_id: which cluster performed the allocation
//             - L1 address: address of allocated memory in the cluster's L1
//             - size: size of the allocation
//             - valid flag: marks whether the entry is active/valid

//     Additional Tasks:
//         - Define a known-aligned HBM base address for global_alloc_table.
//         - Add synchronization/barriers to avoid stale reads/writes.
//         - Write the DMA transfer logic using the iDMA controller.
//         - Handle edge cases (e.g., table full, duplicate cluster entries, invalid entry cleanup).
//         - Document the extension to the runtime.
// */



#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "flex_alloc.h" // Project-specific allocator header

// ---------------------------
// Cluster/Memory Configuration
// ---------------------------

// Number of clusters in the system
#define NUM_CLUSTERS 2

// Number of processing cores per cluster
#define CORES_PER_CLUSTER 2

// Max number of cluster-wide free memory blocks tracked per cluster
#define MAX_CLUSTER_WIDE_FREE_BLOCKS 64

// --------------------------------------------------------------
// Per-cluster, per-core allocator state (in high-bandwidth memory)
// --------------------------------------------------------------

// Each core in each cluster has its own L1 allocator state, placed in HBM for bandwidth and accessibility reasons.

// Per-core L1 allocators for each cluster
__attribute__((section(".hbm")))
alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

// --------------------------------
// Cluster-level synchronization API
// --------------------------------

// Acquire the spinlock for a cluster (for concurrency protection)
void lock(int cluster_id);

// Release the spinlock for a cluster
void unlock(int cluster_id);

// ------------------------------------------
// Cluster-wide free block tracking structures
// ------------------------------------------

// Metadata for a block that is free (available) in all cores of a cluster, for efficient cross-core memory management.
typedef struct {
    void *start_addr;    // Starting address of the free memory block
    uint32_t size;       // Size (in bytes) of the free memory block
} free_block_info_t;

// Cluster-wide free block records, used for tracking blocks that are free across all cores in a cluster
__attribute__((section(".hbm")))
free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];

// Number of cluster-wide free blocks per cluster
__attribute__((section(".hbm")))
uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// Simple spinlock array, one lock per cluster, to serialize access to shared structures
volatile int cluster_lock[NUM_CLUSTERS] = {0};

// -----------------------
// Allocator Management API
// -----------------------

// Initialize all per-core L1 allocators for all clusters, given base addresses and memory sizes per core.
// Also resets cluster-wide free block tracking and synchronization primitives.
void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]);

// Allocate a memory block of the given size from a specific core's L1 allocator.
// Returns a pointer to the allocated memory or NULL on failure.
void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size);

// Free a memory block previously allocated from a specific core's L1 allocator.
void flex_l1_block_free(int cluster_id, int core_id, void *ptr);

// Scan all per-core allocators in a cluster and refresh the list of blocks that are free in every core (cluster-wide).
void update_cluster_wide_free_blocks(int cluster_id);


// merge .c files
/*
 * Allocator and Free Block State
 * These arrays are placed in a special memory section (HBM = High Bandwidth Memory)
 * to optimize memory bandwidth for allocation operations.
 */

/*
 * Spinlock implementation for cluster-level synchronization.
 * lock() acquires the lock for a given cluster (busy-wait).
 * unlock() releases the lock.
 */
void lock(int cluster_id) {
    while (__sync_lock_test_and_set(&cluster_lock[cluster_id], 1)) { /* spin until lock acquired */ }
}

void unlock(int cluster_id) {
    __sync_lock_release(&cluster_lock[cluster_id]);
}

/*
 * flex_l1_allocators_init
 * Initialize every core's L1 allocator using the provided base addresses and memory sizes.
 * Also clears out cluster-wide free block tracking structures and resets locks.
 *
 * Arguments:
 *   base_addrs -- 2D array mapping [cluster][core] to their base memory addresses
 *   sizes -- 2D array mapping [cluster][core] to their memory region sizes
 */
void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
        for (int core = 0; core < CORES_PER_CLUSTER; ++core) {
            // Initialize the allocator for each core in each cluster
            flex_cluster_alloc_init(&hbm_l1_allocators[c][core], base_addrs[c][core], sizes[c][core]);
        }
    }
    // Zero out cluster-wide free block tracking structures
    memset(hbm_cluster_wide_free_blocks, 0, sizeof(hbm_cluster_wide_free_blocks));
    memset(hbm_cluster_wide_free_block_count, 0, sizeof(hbm_cluster_wide_free_block_count));
    memset((void*)cluster_lock, 0, sizeof(cluster_lock));
}

/*
 * flex_l1_block_alloc
 * Allocate a block of given size from a specific core's L1 allocator.
 * Synchronizes access at the cluster level to avoid race conditions.
 *
 * Arguments:
 *   cluster_id -- cluster index
 *   core_id -- core index within the cluster
 *   size -- size in bytes to allocate
 * Returns:
 *   Pointer to allocated memory or NULL on failure.
 */
void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size) {
    lock(cluster_id);
    void *ptr = domain_malloc(&hbm_l1_allocators[cluster_id][core_id], size);
    update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after allocation
    unlock(cluster_id);
    return ptr;
}

/*
 * flex_l1_block_free
 * Free a previously allocated block from a specific core's L1 allocator.
 * Synchronizes access at the cluster level to avoid race conditions.
 *
 * Arguments:
 *   cluster_id -- cluster index
 *   core_id -- core index within the cluster
 *   ptr -- pointer to memory to free
 */
void flex_l1_block_free(int cluster_id, int core_id, void *ptr) {
    lock(cluster_id);
    domain_free(&hbm_l1_allocators[cluster_id][core_id], ptr);
    update_cluster_wide_free_blocks(cluster_id); // Update cluster-wide free block info after free
    unlock(cluster_id);
}

/*
 * update_cluster_wide_free_blocks
 * Scans all cores in the given cluster to find memory blocks that are free in every core
 * (that is, "cluster-wide" free). These blocks are recorded for efficient reuse.
 *
 * Algorithm:
 *   For each block in core 0's allocator:
 *     Check if a block at the same address/size exists and is free in all other cores.
 *     If so, add it to the cluster-wide free block info.
 *   Stops adding if MAX_CLUSTER_WIDE_FREE_BLOCKS is reached.
 *
 * Arguments:
 *   cluster_id -- cluster index to update
 */
void update_cluster_wide_free_blocks(int cluster_id) {
    hbm_cluster_wide_free_block_count[cluster_id] = 0;
    alloc_t *core0_alloc = &hbm_l1_allocators[cluster_id][0];
    alloc_block_t *blk0 = core0_alloc->first_block;
    while (blk0) {
        int is_free_all = 1;
        // Check if this block is free in all other cores
        for (int core = 1; core < CORES_PER_CLUSTER; ++core) {
            alloc_t *other_alloc = &hbm_l1_allocators[cluster_id][core];
            alloc_block_t *blk_other = other_alloc->first_block;
            int found = 0;
            while (blk_other) {
                // Compare block address and size for match
                if ((blk_other->size == blk0->size) && ((uintptr_t)blk_other == (uintptr_t)blk0)) {
                    found = 1;
                    break;
                }
                blk_other = blk_other->next;
            }
            if (!found) {
                is_free_all = 0;
                break;
            }
        }
        // If block is free in all cores, record it for cluster-wide use
        if (is_free_all && hbm_cluster_wide_free_block_count[cluster_id] < MAX_CLUSTER_WIDE_FREE_BLOCKS) {
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].start_addr = (void*)blk0;
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].size = blk0->size;
            hbm_cluster_wide_free_block_count[cluster_id]++;
        }
        blk0 = blk0->next;
    }
}
