/* --- Kill compiler-inserted traps (RISC-V emits ebreak) --- */
static inline __attribute__((always_inline,noreturn)) void __socd_halt_forever(void) {
    for (;;) { __asm__ volatile ("" ::: "memory"); }
}
#ifdef __GNUC__
  #undef  __builtin_trap
  #undef  __builtin_unreachable
  #define __builtin_trap()        __socd_halt_forever()
  #define __builtin_unreachable() __socd_halt_forever()
#endif
/* ---------------------------------------------------------- */

#include "flex_runtime.h"
#include "flex_printf.h"
#include "flex_dma_pattern.h"
#include "flex_group_barrier.h"
#include "soc_daml.h"

/* Clean ordered prints so we can read logs */
static void hello_ordered_all(void)
{
    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        printf("HELLO-ORDERED\n");
    }
    flex_global_barrier_xy();

    for (int cid = 0; cid < (int)ARCH_NUM_CLUSTER; ++cid) {
        for (int k = 0; k < (int)ARCH_NUM_CORE_PER_CLUSTER; ++k) {
            if (flex_get_cluster_id() == (uint32_t)cid &&
                flex_get_core_id()    == (uint32_t)k) {
                printf("    [HELLO-ORDERED] C%u/K%u says hi\n",
                       flex_get_cluster_id(), flex_get_core_id());
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();
}

/* Debug-print the snapshot head block (first free block) */
static void dump_first_free_blocks(void)
{
    if (!(flex_is_first_core() && flex_get_cluster_id() == 0)) return;

    printf("[SNAPSHOT-FIRST] Per (cluster,core) first free block:\n");
    for (uint32_t c = 0; c < ARCH_NUM_CLUSTER; ++c) {
        for (uint32_t k = 0; k < ARCH_NUM_CORE_PER_CLUSTER; ++k) {
            const uint32_t n = soc_daml_get_core_free_count(c, k);
            if (n == 0) {
                printf("    C%u/K%u: <none>\n", c, k);
            } else {
                const daml_block_t *b = soc_daml_get_core_free_list(c, k);
                printf("    C%u/K%u: [0x%08x .. 0x%08x) size=0x%08x\n",
                       c, k,
                       (uint32_t)(uintptr_t)b[0].addr,
                       (uint32_t)((uintptr_t)b[0].addr + b[0].size),
                       b[0].size);
            }
        }
    }
}

/* Print the relaxed per-core system-common result */
static void dump_system_common_relaxed(void)
{
    if (!(flex_is_first_core() && flex_get_cluster_id() == 0)) return;

    printf("[RELAXED] System-common per core (across clusters):\n");
    for (uint32_t k = 0; k < ARCH_NUM_CORE_PER_CLUSTER; ++k) {
        const uint32_t v = soc_daml_get_system_common_relaxed_valid((int)k);
        if (!v) {
            printf("    core K%u : <none>\n", k);
        } else {
            const daml_block_t *b = soc_daml_get_system_common_relaxed((int)k);
            printf("    core K%u : [0x%08x .. 0x%08x) size=0x%08x\n",
                   k,
                   (uint32_t)(uintptr_t)b->addr,
                   (uint32_t)((uintptr_t)b->addr + b->size),
                   b->size);
        }
    }
}

int main(void)
{
    uint32_t eoc_val = 0;

    flex_barrier_xy_init();
    flex_global_barrier_xy();

    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        printf("[SystemInfo]: num_cluster_x = %d, num_cluster_y = %d\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y);
        printf("[BOOT] Using %dx%d clusters, %d cores/cluster\n",
               ARCH_NUM_CLUSTER_X, ARCH_NUM_CLUSTER_Y, ARCH_NUM_CORE_PER_CLUSTER);
        printf("[BOOT] Building allocator maps...\n");
    }
    flex_global_barrier_xy();

    /* Build per-(cluster,core) L1 arenas */
    static void *base_addrs[ARCH_NUM_CLUSTER][ARCH_NUM_CORE_PER_CLUSTER];
    static uint32_t sizes[ARCH_NUM_CLUSTER][ARCH_NUM_CORE_PER_CLUSTER];

    /* We use identical partitioning on every cluster (same L1 layout) */
    const uint32_t HEAP_BASE = ARCH_CLUSTER_HEAP_BASE;
    const uint32_t HEAP_END  = ARCH_CLUSTER_HEAP_END;
    const uint32_t L1_SIZE   = (HEAP_END - HEAP_BASE);
    const uint32_t K         = ARCH_NUM_CORE_PER_CLUSTER;
    const uint32_t per_core  = L1_SIZE / K;  /* simple even split */

    for (uint32_t c = 0; c < ARCH_NUM_CLUSTER; ++c) {
        for (uint32_t k = 0; k < ARCH_NUM_CORE_PER_CLUSTER; ++k) {
            uint32_t start = HEAP_BASE + k * per_core;
            uint32_t end   = (k == ARCH_NUM_CORE_PER_CLUSTER - 1)
                           ? HEAP_END
                           : (start + per_core);
            base_addrs[c][k] = (void*)(uintptr_t)start;
            sizes[c][k]      = (end - start);
        }
    }

    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        printf("[BOOT] Global metadata zeroed.\n");
        printf("[BOOT] Initializing allocators in HBM...\n");
    }
    flex_global_barrier_xy();

    /* Show arenas cluster-by-cluster for readability */
    if (flex_is_first_core()) {
        for (uint32_t c = 0; c < ARCH_NUM_CLUSTER; ++c) {
            if (flex_get_cluster_id() == c) {
                printf("[BOOT] Init allocators (cluster %u)\n", c);
                for (uint32_t k = 0; k < ARCH_NUM_CORE_PER_CLUSTER; ++k) {
                    printf("    [BOOT] C%u/K%u arena: [0x%08x .. 0x%08x) size=0x%08x\n",
                           c, k,
                           (uint32_t)(uintptr_t)base_addrs[c][k],
                           (uint32_t)((uintptr_t)base_addrs[c][k] + sizes[c][k]),
                           sizes[c][k]);
                }
            }
            flex_global_barrier_xy();
        }
    }
    flex_global_barrier_xy();

    /* Initialize allocators (one time) */
    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        soc_daml_init_allocators(base_addrs, sizes);
    }
    flex_global_barrier_xy();

    /* Upload snapshots (per core free lists) */
    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        printf("[BOOT] Uploading per-core free lists...\n");
    }
    flex_global_barrier_xy();

    for (uint32_t c = 0; c < ARCH_NUM_CLUSTER; ++c) {
        for (uint32_t k = 0; k < ARCH_NUM_CORE_PER_CLUSTER; ++k) {
            if (flex_is_first_core() && flex_get_cluster_id() == 0) {
                printf("[BOOT] Snapshot begin C%u/K%u\n", c, k);
            }
            soc_daml_upload_free_list((int)c, (int)k);
            if (flex_is_first_core() && flex_get_cluster_id() == 0) {
                const uint32_t n = soc_daml_get_core_free_count((int)c, (int)k);
                printf("[SNAPSHOT] Uploaded C%u/K%u blocks=%u\n", c, k, n);
                printf("[BOOT] Snapshot end   C%u/K%u\n", c, k);
            }
        }
    }
    flex_global_barrier_xy();

    /* Show the head block we will intersect (nice sanity check) */
    dump_first_free_blocks();
    flex_global_barrier_xy();

    /* Build RELAXED system-common per-core (across clusters) */
    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        printf("[BOOT] Building system-common (RELAXED per core)...\n");
        soc_daml_build_system_common_relaxed_per_core();
    }
    flex_global_barrier_xy();

    /* Print the relaxed intersection results */
    dump_system_common_relaxed();
    flex_global_barrier_xy();

    /* Ordered hello to show progress is clean */
    hello_ordered_all();

    if (flex_is_first_core() && flex_get_cluster_id() == 0) {
        flex_timer_start();
        flex_timer_end();
    }
    flex_global_barrier_xy();

    flex_eoc(eoc_val);
    return 0;
}

/* ============================================================
 *                 NO-BREAK SHIMS (no semihosting)
 * Provide strong symbols so the linker won’t pull in the
 * semihosting/newlib versions that contain 'ebreak'.
 * ============================================================ */
__attribute__((weak, noreturn)) void abort(void)               { for (;;){ } }
__attribute__((weak, noreturn)) void __assert_fail(const char*,
                                                   const char*,
                                                   unsigned int,
                                                   const char*) { for (;;){ } }
__attribute__((weak, noreturn)) void __stack_chk_fail(void)    { for (;;){ } }
__attribute__((weak, noreturn)) void _exit(int x)              { (void)x; for (;;){ } }

/* Some runtimes call into these; make them harmless. */
__attribute__((weak)) int raise(int sig)            { (void)sig; return 0; }
__attribute__((weak)) int kill(int pid, int sig)    { (void)pid; (void)sig; return 0; }
__attribute__((weak)) int getpid(void)              { return 1; }

/* Optional C++ pure-virtual guard (even if you don't use C++). */
__attribute__((weak)) void __cxa_pure_virtual(void) { for (;;){ } }
