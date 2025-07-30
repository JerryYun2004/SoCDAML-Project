#pragma once

#include <stddef.h>
#include <stdint.h>

#define NUM_CLUSTERS 2
#define CORES_PER_CLUSTER 2
#define MAX_BLOCKS_PER_CORE 32
#define MAX_CLUSTER_WIDE_FREE_BLOCKS 64

// Metadata for each L1 block.
typedef struct {
    void *l1_addr;
    uint32_t size;
    uint8_t in_use;   // 1 = allocated, 0 = free
    uint8_t canary;   // For corruption detection
} l1_block_info_t;

// Per-core metadata.
typedef struct {
    l1_block_info_t blocks[MAX_BLOCKS_PER_CORE];
    uint32_t num_blocks;
} core_l1_metadata_t;

// Info for cluster-wide free blocks.
typedef struct {
    void *start_addr;
    uint32_t size;
} free_block_info_t;

// Per-core metadata in HBM
__attribute__((section(".hbm")))
extern core_l1_metadata_t hbm_l1_metadata[NUM_CLUSTERS][CORES_PER_CLUSTER];

// Per-cluster cluster-wide free blocks in HBM
__attribute__((section(".hbm")))
extern free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];
__attribute__((section(".hbm")))
extern uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

// Lock per cluster for concurrency (simple spinlock for example)
extern volatile int cluster_lock[NUM_CLUSTERS];

// API functions
void flex_l1_init(void);
void flex_l1_block_alloc(int cluster_id, int core_id, void *addr, uint32_t size);
void flex_l1_block_free(int cluster_id, int core_id, void *addr);
void update_cluster_wide_free_blocks(int cluster_id);

void update_cluster_wide_free_blocks(int cluster_id)
{
    hbm_cluster_wide_free_block_count[cluster_id] = 0;

    // For each block in core 0, check if it is free in all other cores
    for (int b = 0; b < hbm_l1_metadata[cluster_id][0].num_blocks; ++b) {
        l1_block_info_t *ref_blk = &hbm_l1_metadata[cluster_id][0].blocks[b];
        if (!ref_blk->l1_addr || ref_blk->in_use) continue;

        int is_free_all = 1;
        for (int core = 1; core < CORES_PER_CLUSTER; ++core) {
            int found = 0;
            for (int bb = 0; bb < hbm_l1_metadata[cluster_id][core].num_blocks; ++bb) {
                l1_block_info_t *blk = &hbm_l1_metadata[cluster_id][core].blocks[bb];
                if (blk->l1_addr == ref_blk->l1_addr &&
                    blk->size == ref_blk->size &&
                    !blk->in_use) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                is_free_all = 0;
                break;
            }
        }
        if (is_free_all && hbm_cluster_wide_free_block_count[cluster_id] < MAX_CLUSTER_WIDE_FREE_BLOCKS) {
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].start_addr = ref_blk->l1_addr;
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].size = ref_blk->size;
            hbm_cluster_wide_free_block_count[cluster_id]++;
        }
    }
}










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

// Put this table in HBM
volatile intercluster_alloc_table_t *global_alloc_table = (intercluster_alloc_table_t *) 0x8XXXXXXX;  // HBM base address


// Modify domain_malloc() or wrap it with a new function

void *intercluster_malloc(alloc_t *alloc, uint32_t cluster_id, uint32_t size) {
    void *ptr = domain_malloc(alloc, size);
    if (!ptr) return NULL;

    // Register the allocation in the global table
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


// On another cluster, retrieve the address for DMA:

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
start_idma_transfer(src_addr, dst_addr, size);  // Implemented via DMA engine


/*
    Additional Tasks:
        Define HBM base address	        - Use a known shared HBM address (aligned) to hold global_alloc_table
        Add synchronization	            - Ensure memory barriers (__sync_something()) to avoid stale reads
        Write DMA transfer logic        - Use the iDMA controller to transfer memory from cluster to cluster
        Handle edge cases               - Handle table full, duplicate cluster entries, invalid entries cleanup
        Document                        - Clearly describe how intercluster_malloc extends the runtime
*/