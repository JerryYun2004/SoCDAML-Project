#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"
#include "flex_alloc.h"
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Configuration
 * -------------------------------------------------------------------------- */

/* Size of the “broadcasted” copy (will be clamped to common range and 4-byte aligned) */
#ifndef BROADCAST_BYTES
#define BROADCAST_BYTES  2048u
#endif

/* Keep preload sizes aligned to SLOT_SIZE so the bitmap reflects them cleanly. */
#ifndef PRELOAD_MIN_SLOTS
#define PRELOAD_MIN_SLOTS  2u    /* >= 2 KiB if SLOT_SIZE==1 KiB */
#endif
#ifndef PRELOAD_MAX_SLOTS
#define PRELOAD_MAX_SLOTS  9u
#endif

/* --------------------------------------------------------------------------
 * Simple helpers
 * -------------------------------------------------------------------------- */

static void fill_pattern_u32(uint32_t *dst, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i) {
        dst[i] = seed ^ (0x9E3779B9u * i);
    }
}

static int verify_pattern_u32(const uint32_t *ptr, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i) {
        uint32_t expect = seed ^ (0x9E3779B9u * i);
        if (ptr[i] != expect) {
            return (int)i; /* mismatch index */
        }
    }
    return -1; /* OK */
}

/* Replace this with your real iDMA call when available. */
static void local_copy_from_hbm(void *l1_dst, const void *hbm_src, uint32_t n_bytes)
{
    const uint32_t *s = (const uint32_t *)hbm_src;
    uint32_t *d = (uint32_t *)l1_dst;
    uint32_t n = n_bytes >> 2;
    uint32_t i;
    for (i = 0; i < n; ++i) {
        d[i] = s[i];
    }
}

/* Ordered printing so logs are readable */
static void print_in_cluster_core_order(const char *tag)
{
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("%s\n", tag);
    }
    flex_global_barrier_xy();

    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            uint32_t kid;
            for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
                flex_global_barrier_xy();
                if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                    printf("    [%s] C%u/K%u says hi\n", tag, cid, kid);
                }
                flex_global_barrier_xy();
            }
        }
    }

    flex_global_barrier_xy();
}

/* --------------------------------------------------------------------------
 * L1 preload: allocate different amounts per cluster/core to force divergence
 * -------------------------------------------------------------------------- */

static inline uint32_t daml_min_u32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

/* Make a deterministic, cluster/core–dependent size in SLOT_SIZE units */
static uint32_t choose_preload_slots(uint32_t cluster_id, uint32_t core_id)
{
    /* Spread sizes with a simple, bounded formula:
       base varies with cluster, then add a per-core offset */
    uint32_t base = PRELOAD_MIN_SLOTS + (cluster_id % (PRELOAD_MAX_SLOTS - PRELOAD_MIN_SLOTS + 1u));
    uint32_t off  = (core_id % 3u);  /* 0,1,2 */
    uint32_t want = base + off;

    if (want < PRELOAD_MIN_SLOTS) want = PRELOAD_MIN_SLOTS;
    if (want > PRELOAD_MAX_SLOTS) want = PRELOAD_MAX_SLOTS;
    return want;
}

/* Allocate a few blocks in this cluster’s L1 before snapshot.
   Run by K0 of the cluster (single writer), but it allocates *in each core’s arena*
   using the per-core allocators g_hbm_l1_allocators[c][k]. */
static void preload_l1_this_cluster(uint32_t cluster_id)
{
    if (flex_get_core_id() != 0) return; /* only core 0 performs preloading for the cluster */

    /* Keep a small array of returned pointers so we can optionally free one to create a hole. */
    void *ptrs[ARCH_NUM_CORE_PER_CLUSTER];
    uint32_t ptrs_n = 0u;

    /* For each core’s arena, allocate N slots (converted to bytes), clamped to arena size. */
    {
        uint32_t kid;
        for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {

            /* Figure out this core’s arena [base,size] so we can clamp safely. */
            uint32_t arena_base, arena_size;
            /* Use the same math as soc_daml_init_allocators_this_cluster() */
            {
                const uint32_t heap_base = (uint32_t)ARCH_CLUSTER_HEAP_BASE;
                const uint32_t heap_end  = (uint32_t)ARCH_CLUSTER_HEAP_END;
                const uint32_t heap_size = (heap_end > heap_base) ? (heap_end - heap_base) : 0u;

                uint32_t share = (ARCH_NUM_CORE_PER_CLUSTER > 0u)
                               ? (heap_size / ARCH_NUM_CORE_PER_CLUSTER)
                               : 0u;

                const uint32_t align = (uint32_t)sizeof(alloc_block_t);
                share = (share / align) * align;

                arena_base = heap_base + kid * share;
                if (kid == (ARCH_NUM_CORE_PER_CLUSTER - 1u)) {
                    arena_size = heap_end - arena_base;
                } else {
                    arena_size = share;
                }
                arena_size = (arena_size / align) * align;
            }

            uint32_t want_slots = choose_preload_slots(cluster_id, kid);
            uint32_t want_bytes = want_slots * SLOT_SIZE;

            /* Clamp to something < arena_size so we keep some free space. */
            if (arena_size > SLOT_SIZE) {
                uint32_t max_bytes = arena_size - SLOT_SIZE; /* leave one slot free at least */
                if (want_bytes > max_bytes) want_bytes = max_bytes;
            } else {
                want_bytes = 0u;
            }

            if (want_bytes >= sizeof(alloc_block_t)) {
                void *p = domain_malloc(&g_hbm_l1_allocators[cluster_id][kid], want_bytes);
                if (p != 0) {
                    ptrs[ptrs_n++] = p;
                    printf("[PRELOAD] C%u/K%u allocated %u slots (%u bytes) at 0x%08x (arena [0x%08x..0x%08x))\n",
                           cluster_id, kid, (unsigned)want_slots, (unsigned)want_bytes,
                           (unsigned)(uint32_t)(uintptr_t)p,
                           (unsigned)arena_base, (unsigned)(arena_base + arena_size));
                } else {
                    printf("[PRELOAD][WARN] C%u/K%u failed to alloc %u bytes\n",
                           cluster_id, kid, (unsigned)want_bytes);
                }
            }
        }
    }

    /* Optional: create a “hole” by freeing one middle allocation on some clusters. */
    if ((cluster_id % 4u) == 1u) {
        if (ptrs_n > 1u) {
            /* free the second entry to punch a gap */
            domain_free(&g_hbm_l1_allocators[cluster_id][1], ptrs[1]);
            printf("[PRELOAD] C%u/K1 freed one block to create a hole\n", cluster_id);
        }
    }
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(void)
{
    /* Shared state for verification results (in L3, visible to all clusters) */
    static volatile int g_cluster_verify_status[ARCH_NUM_CLUSTER];

    /* HBM source pointer shared by all clusters */
    static void *g_hbm_src = 0;

    uint32_t eoc_val = 0;

    /* Boot sync & timer */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        flex_timer_start();

        /* clear top-level system-common outputs */
        g_hbm_system_common_count = 0u;
        daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));

        /* init HBM allocator once (C0/K0) */
        soc_daml_init_hbm_allocator();

        /* zero verify status */
        {
            uint32_t i;
            for (i = 0; i < ARCH_NUM_CLUSTER; ++i) {
                g_cluster_verify_status[i] = 0;
            }
        }
    }
    flex_global_barrier_xy();

    soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);

    /* Per-cluster allocator init on owner */
    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            flex_global_barrier_xy();
            if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
                printf("[BOOT] Init allocators (cluster %u)\n", cid);
                soc_daml_init_allocators_this_cluster(cid);
                printf("[BOOT] Done (cluster %u)\n", cid);
            }
            flex_global_barrier_xy();
        }
    }

    /* --- NEW: preload L1 (different per cluster/core) BEFORE snapshots --- */
    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            flex_global_barrier_xy();
            if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
                preload_l1_this_cluster(cid);
            }
            flex_global_barrier_xy();
        }
    }

    /* Snapshots: each (cid,kid) does its own */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            uint32_t kid;
            for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
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
    }

    /* Build per-core bitmaps, then cluster-free (OR), then system-common (AND) */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building per-core FREE bitmaps (S=0x%08x)...\n", (unsigned)SLOT_SIZE);
    }
    flex_global_barrier_xy();

    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            uint32_t kid;
            for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
                flex_global_barrier_xy();
                if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                    soc_daml_build_core_bitmap(cid, kid);
                }
                flex_global_barrier_xy();
            }
        }
    }

    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building CLUSTER-FREE bitmaps (OR across cores)...\n");
    }
    flex_global_barrier_xy();

    {
        uint32_t cid;
        for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == 0) {
                soc_daml_build_cluster_free_bitmap(cid);
            }
            flex_global_barrier_xy();
        }
    }

    /* System = AND across clusters + compress to ranges */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building SYSTEM-COMMON (AND across clusters)...\n");
        soc_daml_build_system_common_bitmap();
        soc_daml_system_common_bitmap_to_ranges();
        {
            uint32_t n = soc_daml_get_system_common_count();
            printf("[BOOT] System common ranges = %u\n", n);
            {
                const daml_block_t *lst = soc_daml_get_system_common();
                uint32_t to_show = (n > 8u) ? 8u : n;
                uint32_t i;
                for (i = 0; i < to_show; ++i) {
                    uint32_t lo = (uint32_t)(uintptr_t)lst[i].addr;
                    uint32_t hi = lo + lst[i].size;
                    printf("    [COMMON %u] [0x%08x .. 0x%08x) size=0x%08x\n", i, lo, hi, lst[i].size);
                }
            }
        }
    }
    flex_global_barrier_xy();

    /* ----------------------------------------------------------------------
     * DMA-style BROADCAST VALIDATION (emulated copy)
     * ---------------------------------------------------------------------- */
    {
        uint32_t sys_n = soc_daml_get_system_common_count();
        if (sys_n > 0u) {
            const daml_block_t *sys_common = soc_daml_get_system_common();
            uint32_t common_base = (uint32_t)(uintptr_t)sys_common[0].addr;
            uint32_t common_size = sys_common[0].size;

            uint32_t len = BROADCAST_BYTES;
            if (len > common_size) {
                len = common_size;
            }
            len = len & ~0x3u; /* 4-byte align for word copy */

            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[TEST] DMA broadcast demo:\n");
                printf("       common L1 dst = 0x%08x, size available = 0x%08x, copy len = 0x%08x\n",
                       common_base, common_size, len);
            }
            flex_global_barrier_xy();

            /* Allocate and fill HBM source on C0/K0 */
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                g_hbm_src = flex_hbm_malloc(len);
                if (g_hbm_src == 0) {
                    printf("[TEST][ERR] HBM malloc failed for len=0x%08x\n", len);
                } else {
                    fill_pattern_u32((uint32_t *)g_hbm_src, (len >> 2), 0xA5A5A5A5u);
                    printf("[TEST] HBM src @ %p filled with pattern\n", g_hbm_src);
                }
            }
            flex_global_barrier_xy();

            if (g_hbm_src == 0) {
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                    printf("[TEST][ABORT] No HBM source; skipping broadcast test\n");
                }
            } else {
                /* One transfer per cluster (core 0), to the same L1 address. */
                if (flex_get_core_id() == 0) {
                    local_copy_from_hbm((void *)(uintptr_t)common_base, g_hbm_src, len);
                }
                flex_global_barrier_xy();

                /* Verify on each cluster (core 0) and store status. */
                if (flex_get_core_id() == 0) {
                    int bad_idx = verify_pattern_u32(
                        (const uint32_t *)(uintptr_t)common_base,
                        (len >> 2),
                        0xA5A5A5A5u);
                    g_cluster_verify_status[flex_get_cluster_id()] = (bad_idx == -1) ? 1 : -(bad_idx + 1);
                }
                flex_global_barrier_xy();

                /* Print a line for EVERY cluster from C0/K0. */
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                    uint32_t cid;
                    for (cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
                        int st = g_cluster_verify_status[cid];
                        if (st == 1) {
                            printf("[TEST][OK ] Cluster %u verified %u bytes at 0x%08x\n",
                                   cid, (unsigned)len, common_base);
                        } else if (st == 0) {
                            printf("[TEST][ERR] Cluster %u status unset\n", cid);
                        } else {
                            /* negative => -(bad_idx+1) */
                            uint32_t bad_idx = (uint32_t)(-st - 1);
                            uint32_t err_addr = common_base + (bad_idx << 2);
                            printf("[TEST][ERR] Cluster %u mismatch at word %u (addr=0x%08x)\n",
                                   cid, bad_idx, err_addr);
                        }
                    }
                }
                flex_global_barrier_xy();

                /* Free HBM source on C0/K0. */
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                    flex_hbm_free(g_hbm_src);
                    g_hbm_src = 0;
                }
                flex_global_barrier_xy();
            }
        } else {
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[TEST] No system-common range; broadcast test skipped.\n");
            }
            flex_global_barrier_xy();
        }
    }

    /* Optional: ordered hello */
    print_in_cluster_core_order("HELLO-ORDERED");

    /* Done */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        flex_timer_end();
    }
    flex_global_barrier_xy();
    flex_eoc(eoc_val);
    return 0;
}
