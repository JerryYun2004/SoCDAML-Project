#pragma once

#include <stddef.h>
#include <stdint.h>
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
__attribute__((section(".hbm")))
extern alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

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

// Array of cluster-wide free block metadata per cluster, placed in HBM for global access.
__attribute__((section(".hbm")))
extern free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];

// Number of cluster-wide free blocks currently tracked for each cluster (in HBM).
__attribute__((section(".hbm")))
extern uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// Array of simple spinlocks, one per cluster, for basic concurrency control.
extern volatile int cluster_lock[NUM_CLUSTERS];

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

/*
    Key Challenges:
        - Each cluster has its own private L1 allocator, isolated from others.
        - Other clusters cannot directly access a cluster's allocator state (since it's stored in private L1 memory).
        - For inter-cluster DMA (Direct Memory Access), the source address must be known and accessible by other clusters.
    To do:
        - Store allocated memory metadata in a globally accessible structure (e.g., in HBM).
        - Extend the domain_malloc() runtime to record allocations in this global table.
        - Add logic so a cluster can lookup this table and perform DMA transfers based on recorded allocations.
*/

/*
    Design for Inter-cluster Accessible Allocation Table:
        - Place the table in HBM (so all clusters can read/write it).
        - Each allocation record in the table contains:
            - cluster_id: which cluster performed the allocation
            - L1 address: address of allocated memory in the cluster's L1
            - size: size of the allocation
            - valid flag: marks whether the entry is active/valid
*/

/*
    Pseudocode for Intercluster Allocator Table (see below for C struct example):
        #define MAX_ALLOC_ENTRIES 32

        typedef struct {
            uint32_t cluster_id;
            uint32_t addr_l1;
            uint32_t size;
            uint8_t  valid;
        } intercluster_alloc_entry_t;

        typedef struct {
            intercluster_alloc_entry_t entries[MAX_ALLOC_ENTRIES];
        } intercluster_alloc_table_t;

        // Place this table in HBM:
        volatile intercluster_alloc_table_t *global_alloc_table = (intercluster_alloc_table_t *) 0x8XXXXXXX;  // HBM base address

        // Allocation wrapper:
        void *intercluster_malloc(alloc_t *alloc, uint32_t cluster_id, uint32_t size) {
            void *ptr = domain_malloc(alloc, size);
            if (!ptr) return NULL;

            // Register in global table
            for (int i = 0; i < MAX_ALLOC_ENTRIES; i++) {
                if (global_alloc_table->entries[i].valid == 0) {
                    global_alloc_table->entries[i].cluster_id = cluster_id;
                    global_alloc_table->entries[i].addr_l1 = (uint32_t) ptr;
                    global_alloc_table->entries[i].size = size;
                    global_alloc_table->entries[i].valid = 1;
                    break;
                }
            }
            return ptr;
        }

        // Retrieve an address from the global table for DMA
        uint32_t get_allocated_address(uint32_t cluster_id) {
            for (int i = 0; i < MAX_ALLOC_ENTRIES; i++) {
                if (global_alloc_table->entries[i].valid &&
                    global_alloc_table->entries[i].cluster_id == cluster_id) {
                    return global_alloc_table->entries[i].addr_l1;
                }
            }
            return 0; // not found
        }

        // Use iDMA to request data from another cluster
        uint32_t src_addr = get_allocated_address(/* cluster 0 */ 0);
        uint32_t dst_addr = flex_l1_malloc(size);  // locally allocated
        start_idma_transfer(src_addr, dst_addr, size);  // Implement the DMA engine logic

    Additional Tasks:
        - Define a known-aligned HBM base address for global_alloc_table.
        - Add synchronization/barriers to avoid stale reads/writes.
        - Write the DMA transfer logic using the iDMA controller.
        - Handle edge cases (e.g., table full, duplicate cluster entries, invalid entry cleanup).
        - Document the extension to the runtime.
*/
