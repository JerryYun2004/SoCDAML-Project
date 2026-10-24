/* main_timer.c — minimal timer-stamp smoke test
 *
 * Purpose:
 *   Emit exactly one pair of flex_timer_start()/flex_timer_end() from C[0,0] core 0
 *   with no other printing, DMA, or allocation. If your simulator is wired to
 *   auto-log these MMIO writes, you should see a timing line; otherwise, this
 *   will run silently.
 */

#include <stdint.h>
#include "flex_runtime.h"   /* flex_barrier_xy_init, flex_global_barrier_xy, flex_eoc, timers */

static inline void burn_cycles(uint32_t iters)
{
    volatile uint32_t acc = 0u;
    for (uint32_t i = 0; i < iters; ++i) {
        acc += i;
    }
    (void)acc;
}

int main(void)
{
    /* Basic bring-up */
    flex_barrier_xy_init();
    flex_global_barrier_xy();

    const uint32_t cid  = flex_get_cluster_id();
    const uint32_t core = flex_get_core_id();
    const FlexPosition P = get_pos(cid);   /* .x in [0..3], .y in [0..3] */

    /* Only let C[0,0] core 0 emit the timer stamps */
    const uint32_t is_timer_master = (uint32_t)((P.x == 0u) & (P.y == 0u) & (core == 0u));

    if (is_timer_master) {flex_timer_start(); }         /* stamp BEGIN */
        burn_cycles(200000u);        /* do a little work so elapsed time is non-zero */
    if (is_timer_master) { flex_timer_end(); }           /* stamp END   */

    /* Everyone rendezvous, then exit */
    flex_global_barrier_xy();
    flex_eoc(0);
    return 0;
}
