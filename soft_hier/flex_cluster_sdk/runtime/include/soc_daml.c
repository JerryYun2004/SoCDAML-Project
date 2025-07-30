#include "soc_daml.h"
#include <string.h>

__attribute__((section(".hbm")))
alloc_t hbm_l1_allocators[NUM_CLUSTERS][CORES_PER_CLUSTER];

__attribute__((section(".hbm")))
free_block_info_t hbm_cluster_wide_free_blocks[NUM_CLUSTERS][MAX_CLUSTER_WIDE_FREE_BLOCKS];
__attribute__((section(".hbm")))
uint32_t hbm_cluster_wide_free_block_count[NUM_CLUSTERS];

volatile int cluster_lock[NUM_CLUSTERS] = {0};

static void lock(int cluster_id) {
    while (__sync_lock_test_and_set(&cluster_lock[cluster_id], 1)) { /* spin */ }
}
static void unlock(int cluster_id) {
    __sync_lock_release(&cluster_lock[cluster_id]);
}

// Initialize every core's allocator using the flex_alloc API
void flex_l1_allocators_init(void *base_addrs[NUM_CLUSTERS][CORES_PER_CLUSTER], uint32_t sizes[NUM_CLUSTERS][CORES_PER_CLUSTER]) {
    for (int c = 0; c < NUM_CLUSTERS; ++c) {
        for (int core = 0; core < CORES_PER_CLUSTER; ++core) {
            flex_cluster_alloc_init(&hbm_l1_allocators[c][core], base_addrs[c][core], sizes[c][core]);
        }
    }
    memset(hbm_cluster_wide_free_blocks, 0, sizeof(hbm_cluster_wide_free_blocks));
    memset(hbm_cluster_wide_free_block_count, 0, sizeof(hbm_cluster_wide_free_block_count));
    memset((void*)cluster_lock, 0, sizeof(cluster_lock));
}

// Allocate a block from the specified core's L1 allocator
void *flex_l1_block_alloc(int cluster_id, int core_id, uint32_t size) {
    lock(cluster_id);
    void *ptr = domain_malloc(&hbm_l1_allocators[cluster_id][core_id], size);
    update_cluster_wide_free_blocks(cluster_id);
    unlock(cluster_id);
    return ptr;
}

// Free a block from the specified core's L1 allocator
void flex_l1_block_free(int cluster_id, int core_id, void *ptr) {
    lock(cluster_id);
    domain_free(&hbm_l1_allocators[cluster_id][core_id], ptr);
    update_cluster_wide_free_blocks(cluster_id);
    unlock(cluster_id);
}

// Aggregate cluster-wide free blocks (blocks free in all cores)
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
        if (is_free_all && hbm_cluster_wide_free_block_count[cluster_id] < MAX_CLUSTER_WIDE_FREE_BLOCKS) {
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].start_addr = (void*)blk0;
            hbm_cluster_wide_free_blocks[cluster_id][hbm_cluster_wide_free_block_count[cluster_id]].size = blk0->size;
            hbm_cluster_wide_free_block_count[cluster_id]++;
        }
        blk0 = blk0->next;
    }
}