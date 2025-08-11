#include "flex_runtime.h"
#include "flex_printf.h"
#include "soc_daml.h"
#include "flex_alloc.h"   /* flex_hbm_malloc(), flex_hbm_free() */
#include <stdint.h>

/* --------------------------------------------------------------------------
 * Broadcast verification status per cluster (HBM-visible so all see it)
 *  -1 : OK
 * >=0: mismatch index
 * -2 : not set yet
 * -------------------------------------------------------------------------- */
__attribute__((section(".hbm")))
static volatile int g_verify_status[ARCH_NUM_CLUSTER];

/* --------------------------------------------------------------------------
 * Simple helpers for the broadcast validation
 * -------------------------------------------------------------------------- */

#ifndef BROADCAST_BYTES
#define BROADCAST_BYTES  2048u   /* 2 KiB by default; will be clamped */
#endif

static void fill_pattern_u32(uint32_t *dst, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i)
    {
        dst[i] = seed ^ (0x9E3779B9u * i);
    }
}

static int verify_pattern_u32(const uint32_t *ptr, uint32_t n_words, uint32_t seed)
{
    uint32_t i;
    for (i = 0; i < n_words; ++i)
    {
        uint32_t expect = seed ^ (0x9E3779B9u * i);
        if (ptr[i] != expect)
        {
            return (int)i; /* mismatch index */
        }
    }
    return -1; /* OK */
}

/* Replace this with your real iDMA call if available. */
static void local_copy_from_hbm(void *l1_dst, const void *hbm_src, uint32_t n_bytes)
{
    /* Copy as 32-bit words; caller aligns n_bytes to a multiple of 4. */
    const uint32_t *s = (const uint32_t *)hbm_src;
    uint32_t *d = (uint32_t *)l1_dst;
    uint32_t n = n_bytes >> 2;
    uint32_t i;
    for (i = 0; i < n; ++i)
    {
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

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                printf("    [%s] C%u/K%u says hi\n", tag, cid, kid);
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(void)
{
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
        /* NEW: initialize HBM allocator once (C0/K0) */
        soc_daml_init_hbm_allocator();

        /* NEW: init verification status table */
        for (uint32_t i = 0; i < ARCH_NUM_CLUSTER; ++i) { g_verify_status[i] = -2; }
    }
    flex_global_barrier_xy();

    soc_daml_set_runtime_dims(ARCH_NUM_CLUSTER, ARCH_NUM_CORE_PER_CLUSTER);

    /* Per-cluster allocator init on owner */
    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        flex_global_barrier_xy();
        if (flex_get_core_id() == 0 && flex_get_cluster_id() == cid) {
            printf("[BOOT] Init allocators (cluster %u)\n", cid);
            soc_daml_init_allocators_this_cluster(cid);
            printf("[BOOT] Done (cluster %u)\n", cid);
        }
        flex_global_barrier_xy();
    }

    /* Snapshots: each (cid,kid) does its own */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
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

    /* Build per-core bitmaps, then cluster-free (OR), then system-common (AND) */
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building per-core FREE bitmaps (S=0x%08x)...\n", (unsigned)SLOT_SIZE);
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        for (uint32_t kid = 0; kid < ARCH_NUM_CORE_PER_CLUSTER; ++kid) {
            flex_global_barrier_xy();
            if (flex_get_cluster_id() == cid && flex_get_core_id() == kid) {
                soc_daml_build_core_bitmap(cid, kid);
            }
            flex_global_barrier_xy();
        }
    }

    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building CLUSTER-FREE bitmaps (OR across cores)...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid) {
        flex_global_barrier_xy();
        if (flex_get_cluster_id() == cid && flex_get_core_id() == 0) {
            soc_daml_build_cluster_free_bitmap(cid);
        }
        flex_global_barrier_xy();
    }

    /* System = AND across clusters + compress to ranges */
    flex_global_barrier_xy();
    if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0) {
        printf("[BOOT] Building SYSTEM-COMMON (AND across clusters)...\n");
        soc_daml_build_system_common_bitmap();
        soc_daml_system_common_bitmap_to_ranges();
        uint32_t n = soc_daml_get_system_common_count();
        printf("[BOOT] System common ranges = %u\n", n);
        const daml_block_t *lst = soc_daml_get_system_common();
        /* print up to a handful */
        uint32_t to_show = (n > 8u) ? 8u : n;
        for (uint32_t i = 0; i < to_show; ++i) {
            uint32_t lo = (uint32_t)(uintptr_t)lst[i].addr;
            uint32_t hi = lo + lst[i].size;
            printf("    [COMMON %u] [0x%08x .. 0x%08x) size=0x%08x\n", i, lo, hi, lst[i].size);
        }
    }
    flex_global_barrier_xy();

    /* ----------------------------------------------------------------------
     * DMA-style BROADCAST VALIDATION (emulated with a word copy)
     * - Choose first system-common range
     * - C0/K0 allocates HBM src and fills a pattern
     * - Each cluster core0 copies HBM->L1 at the SAME L1 address
     * - Each cluster core0 verifies the pattern and stores status
     * - C0/K0 prints all cluster results in order
     * ---------------------------------------------------------------------- */
    {
        uint32_t sys_n = soc_daml_get_system_common_count();
        if (sys_n > 0u)
        {
            const daml_block_t *sys_common = soc_daml_get_system_common();
            uint32_t common_base = (uint32_t)(uintptr_t)sys_common[0].addr;
            uint32_t common_size = sys_common[0].size;

            uint32_t len = BROADCAST_BYTES;
            if (len > common_size)
            {
                len = common_size;
            }
            /* Align to 4 bytes for word copy */
            len = len & ~0x3u;

            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
            {
                printf("[TEST] DMA broadcast demo:\n");
                printf("       common L1 dst = 0x%08x, size available = 0x%08x, copy len = 0x%08x\n",
                       common_base, common_size, len);
                /* reset result table */
                for (uint32_t i = 0; i < ARCH_NUM_CLUSTER; ++i) { g_verify_status[i] = -2; }
            }
            flex_global_barrier_xy();

            /* Allocate and fill HBM source on C0/K0 */
            static void *hbm_src = 0;
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
            {
                hbm_src = flex_hbm_malloc(len);
                if (hbm_src == 0)
                {
                    printf("[TEST][ERR] HBM malloc failed for len=0x%08x\n", len);
                }
                else
                {
                    fill_pattern_u32((uint32_t *)hbm_src, (len >> 2), 0xA5A5A5A5u);
                    printf("[TEST] HBM src @ %p filled with pattern\n", hbm_src);
                }
            }
            flex_global_barrier_xy();

            if (hbm_src == 0)
            {
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
                {
                    printf("[TEST][ABORT] No HBM source; skipping broadcast test\n");
                }
            }
            else
            {
                /* Perform one transfer per cluster (core 0), to the same L1 address. */
                if (flex_get_core_id() == 0)
                {
                    /* Swap with your iDMA call if available. */
                    local_copy_from_hbm((void *)(uintptr_t)common_base, hbm_src, len);
                }
                flex_global_barrier_xy();

                /* Verify on each cluster (core 0) and store status. */
                int local_status = -2;
                if (flex_get_core_id() == 0)
                {
                    local_status = verify_pattern_u32(
                        (const uint32_t *)(uintptr_t)common_base,
                        (len >> 2),
                        0xA5A5A5A5u);
                    g_verify_status[flex_get_cluster_id()] = local_status;
                }
                flex_global_barrier_xy();

                /* Print results in cluster order from C0/K0 for clean logs. */
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
                {
                    for (uint32_t cid = 0; cid < ARCH_NUM_CLUSTER; ++cid)
                    {
                        int st = g_verify_status[cid];
                        if (st == -1)
                        {
                            printf("[TEST][OK ] Cluster %u verified %u bytes at 0x%08x\n",
                                   cid, (unsigned)len, common_base);
                        }
                        else if (st >= 0)
                        {
                            uint32_t err_addr = common_base + ((uint32_t)st << 2);
                            printf("[TEST][ERR] Cluster %u mismatch at word %d (addr=0x%08x)\n",
                                   cid, st, err_addr);
                        }
                        else
                        {
                            /* still unset (-2) – shouldn’t happen if barriers are correct */
                            printf("[TEST][ERR] Cluster %u status unset\n", cid);
                        }
                    }
                }
                flex_global_barrier_xy();

                /* Free HBM source on C0/K0. */
                if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
                {
                    flex_hbm_free(hbm_src);
                }
                flex_global_barrier_xy();
            }
        }
        else
        {
            if (flex_get_cluster_id() == 0 && flex_get_core_id() == 0)
            {
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
