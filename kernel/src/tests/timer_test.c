#include "tests/tests.h"
#include "drivers/pit.h"
#include "kprintf.h"

bool test_timer(void)
{
    uint64_t t0 = timer_ticks();
    uint64_t ms0 = timer_uptime_ms();
    timer_sleep_ms(50);
    uint64_t dt = timer_ticks() - t0;
    uint64_t dms = timer_uptime_ms() - ms0;

    // generous upper bound, vms can be slow to deliver ticks
    bool ok = dt >= 49 && dms >= 50 && dms < 500;
    if (ok)
        pr_ok("timer: slept 50ms, %lu ticks passed\n", dt);
    else
        pr_err("timer: sleep(50) took %lu ticks / %lu ms\n", dt, dms);
    return ok;
}
