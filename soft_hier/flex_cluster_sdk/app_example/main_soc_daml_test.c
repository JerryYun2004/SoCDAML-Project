#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"
#include "flex_alloc.h"
#include <stdint.h>

/* ============================================================================
 * Shared, cross-cluster state: put it in HBM and make it volatile.
 * ============================================================================ */
__attribute__((section(".hbm")))
static volatile int g_cluster_verify_status[ARCH_NUM_CLUSTER];

__attribute__((section(".hbm")))
static volatile void *g_hbm_src = 0;

/* ============================================================================
 * Configuration
 * ============================================================================ */
#ifndef BROADCAST_BYTES
#define BROADCAST_BYTES  (2048u)  /* 2 KiB */
#endif

#ifndef PRELOAD_MIN_SLOTS
#define PRELOAD_MIN_SLOTS  2u
#endif
#ifndef PRELOAD_MAX_SLOTS
#define PRELOAD_MAX_SLOTS  10u
#endif

#define PRELOAD_PARTS_PER_CORE  3u

/* ============================================================================
 * Helpers
 * ============================================================================ */
static inline uint32_t min_u32(uint32_t a, uint32_t b) { return (a < b) ? a : b; }

static void fill_pattern_u32(uint32_t *dst, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i) dst[i] = seed ^ (0x9E3779B9u * i);
}
static int verify_pattern_u32(const uint32_t *ptr, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i) {
        uint32_t expect = seed ^ (0x9E3779B9u * i);
        if (ptr[i] != expect) return (int)i;
    }
    return -1;
}

static void local_copy_from_hbm(void *l1_dst, const void *hbm_src, uint32_t n_bytes)
{
    const uint32_t *s = (const uint32_t *)hbm_src;
    uint32_t *d = (uint32_t *)l1_dst;
    uint32_t n = n_bytes >> 2;
    uint32_t i;
    for (i = 0; i < n; ++i) d[i] = s[i];
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

/* Same arena split that runtime init uses */
static void get_core_arena(uint32_t *out_base,
                           uint32_t *out_size,
                           uint32_t core_id)
{
    const uint32_t heap_base = (uint32_t)ARCH_CLUSTER_HEAP_BASE;
    const uint32_t heap_end  = (uint32_t)ARCH_CLUSTER_HEAP_END;
    const uint32_t heap_size = (heap_end > heap_base) ? (heap_end - heap_base) : 0u;

    uint32_t share = (ARCH_NUM_CORE_PER_CLUSTER > 0u)
                   ? (heap_size / ARCH_NUM_CORE_PER_CLUSTER)
                   : 0u;

    const uint32_t align = (uint32_t)sizeof(alloc_block_t);
    share = (share / align) * align;

    uint32_t base = heap_base + core_id * share;
    uint32_t size = (core_id == (ARCH_NUM_CORE_PER_CLUSTER - 1u)) ? (heap_end - base) : share;
    size = (size / align) * align;

    *out_base = base;
    *out_size = size;
}

/* Deterministic preload size in SLOT_SIZE units */
static uint32_t choose_slots(uint32_t cid, uint32_t kid, uint32_t part_idx)
{
    uint32_t base = PRELOAD_MIN_SLOTS + ((cid + 3u*part_idx) % (PRELOAD_MAX_SLOTS - PRELOAD_MIN_SLOTS + 1u));
    uint32_t off  = (kid * 2u + part_idx) % 3u; /* 0..2 */
    uint32_t want = base + off;
    if (want < PRELOAD_MIN_SLOTS) want = PRELOAD_MIN_SLOTS;
    if (want > PRELOAD_MAX_SLOTS) want = PRELOAD_MAX_SLOTS;
    return want;
}

/* Preload three fragments per core; free the middle on some clusters to create holes */
static void preload_l1_this_cluster(uint32_t cid)
{
    if (flex_get_core_id() != 0) return;

    uint32_t kid;
    for (kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {

        alloc_t *A = (alloc_t *)&g_hbm_l1_allocators[cid][kid];

        uint32_t arena_base, arena_size;
        get_core_arena(&arena_base, &arena_size, kid);

        void *p_head = 0;
        void *p_mid  = 0;
        void *p_tail = 0;

        uint32_t slots_head = choose_slots(cid, kid, 0u);
        uint32_t slots_mid  = choose_slots(cid, kid, 1u);
        uint32_t slots_tail = choose_slots(cid, kid, 2u);

        uint32_t budget_max = (arena_size > SLOT_SIZE) ? (arena_size - SLOT_SIZE) : 0u;

        uint32_t want_head = slots_head * SLOT_SIZE;
        uint32_t want_mid  = slots_mid  * SLOT_SIZE;
        uint32_t want_tail = slots_tail * SLOT_SIZE;

        want_head = min_u32(want_head, budget_max / 2u);
        want_mid  = min_u32(want_mid,  budget_max / 3u);
        want_tail = min_u32(want_tail,  budget_max / 2u);

        if (want_head < sizeof(alloc_block_t)) want_head = 0u;
        if (want_mid  < sizeof(alloc_block_t)) want_mid  = 0u;
        if (want_tail < sizeof(alloc_block_t)) want_tail = 0u;

        if (want_head) p_head = domain_malloc(A, want_head);
        if (want_mid)  p_mid  = domain_malloc(A, want_mid);
        if (want_tail) p_tail = domain_malloc(A, want_tail);

        if (p_head) {
            printf("[PRELOAD] C%u/K%u HEAD  %4u slots (%8u B) @ 0x%08x  arena[0x%08x..0x%08x)\n",
                   cid, kid, (unsigned)slots_head, (unsigned)want_head,
                   (unsigned)(uint32_t)(uintptr_t)p_head,
                   (unsigned)arena_base, (unsigned)(arena_base + arena_size));
        }
        if (p_mid) {
            printf("[PRELOAD] C%u/K%u MID   %4u slots (%8u B) @ 0x%08x\n",
                   cid, kid, (unsigned)slots_mid, (unsigned)want_mid,
                   (unsigned)(uint32_t)(uintptr_t)p_mid);
        }
        if (p_tail) {
            printf("[PRELOAD] C%u/K%u TAIL  %4u slots (%8u B) @ 0x%08x\n",
                   cid, kid, (unsigned)slots_tail, (unsigned)want_tail,
                   (unsigned)(uint32_t)(uintptr_t)p_tail);
        }

        if ((cid % 3u) == 1u && p_mid != 0) {
            domain_free(A, p_mid);
            printf("[PRELOAD] C%u/K%u freed MID to punch a hole\n", cid, kid);
            p_mid = 0;
        }
    }
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(void)
{
    uint32_t eoc_val = 0;

    flex_barrier_xy_init();
    flex_global_barrier_xy();

    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        flex_timer_start();

        /* global init */
        g_hbm_system_common_count = 0u;
        daml_zero_bytes(&g_hbm_system_common[0], (uint32_t)sizeof(g_hbm_system_common));

        /* Zero shared status array (HBM) */
        {
            uint32_t i;
            for (i = 0; i < ARCH_NUM_CLUSTER; ++i) g_cluster_verify_status[i] = 0;
            daml_fence();  /* publish zeros */
        }

        soc_daml_init_hbm_allocator();
    }
    flex_global_barrier_xy();

    soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);

    /* Init per cluster */
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

    /* Preload L1s to create fragmentation */
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

    /* Snapshots */
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

    /* Build bitmaps */
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

    /* =========================================================================
     * HBM -> L1 “broadcast” validation
     * ========================================================================= */
    {
        uint32_t sys_n = soc_daml_get_system_common_count();
        if (sys_n == 0u) {
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[TEST] No system-common range; broadcast test skipped.\n");
            }
            flex_global_barrier_xy();
        } else {
            const daml_block_t *sys_common = soc_daml_get_system_common();
            uint32_t common_base = (uint32_t)(uintptr_t)sys_common[0].addr;
            uint32_t common_size = sys_common[0].size;

            uint32_t len = BROADCAST_BYTES;
            if (len > common_size) len = common_size;
            len = len & ~0x3u;

            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                printf("[TEST] DMA broadcast demo:\n");
                printf("       common L1 dst = 0x%08x, size available = 0x%08x, copy len = 0x%08x\n",
                       common_base, common_size, len);
            }
            flex_global_barrier_xy();

            /* C0/K0 allocates and fills HBM src (HBM-visible pointer) */
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                void *p = flex_hbm_malloc(len);
                g_hbm_src = p;      /* publish raw pointer first */
                daml_fence();       /* publish pointer before contents */

                if (p == 0) {
                    printf("[TEST][ERR] HBM malloc failed for len=0x%08x\n", len);
                } else {
                    fill_pattern_u32((uint32_t *)p, (len >> 2), 0xA5A5A5A5u);
                    daml_fence();   /* publish contents */
                    printf("[TEST] HBM src @ %p filled with pattern\n", p);
                }
            }
            flex_global_barrier_xy();

            if (g_hbm_src == 0) {
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                    printf("[TEST][ABORT] No HBM source; skipping broadcast test\n");
                }
            } else {
                /* One transfer per cluster (core 0) to the same L1 address */
                if (flex_get_core_id() == 0) {
                    local_copy_from_hbm((void *)(uintptr_t)common_base, (const void *)g_hbm_src, len);
                }
                flex_global_barrier_xy();

                /* Core0 of each cluster verifies and records status in HBM */
                if (flex_get_core_id() == 0) {
                    int bad_idx = verify_pattern_u32(
                        (const uint32_t *)(uintptr_t)common_base,
                        (len >> 2),
                        0xA5A5A5A5u);
                    g_cluster_verify_status[flex_get_cluster_id()] = (bad_idx == -1) ? 1 : -(bad_idx + 1);
                    daml_fence();  /* publish this cluster's status */
                }
                flex_global_barrier_xy();

                /* C0/K0 prints a line for EVERY cluster */
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
                            uint32_t bad = (uint32_t)(-st - 1);
                            uint32_t err_addr = common_base + (bad << 2);
                            printf("[TEST][ERR] Cluster %u mismatch at word %u (addr=0x%08x)\n",
                                   cid, bad, err_addr);
                        }
                    }
                }
                flex_global_barrier_xy();

                /* Free HBM source on C0/K0 */
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
                    if (g_hbm_src != 0) {
                        flex_hbm_free((void *)g_hbm_src);
                        g_hbm_src = 0;
                        daml_fence();
                    }
                }
                flex_global_barrier_xy();
            }
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
