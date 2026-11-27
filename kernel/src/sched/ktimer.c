// a list of kernel timers sorted by deadline, checked every tick
#include "sched/ktimer.h"
#include "arch/x86_64/cpu.h"

static struct ktimer *timers;

static void unlink(struct ktimer *t)
{
    for (struct ktimer **pp = &timers; *pp; pp = &(*pp)->next) {
        if (*pp == t) {
            *pp = t->next;
            break;
        }
    }
    t->armed = false;
}

void ktimer_arm(struct ktimer *t, uint64_t deadline_ms)
{
    uint64_t flags = irq_save();
    if (t->armed)
        unlink(t);
    t->deadline = deadline_ms;
    struct ktimer **pp = &timers;
    while (*pp && (*pp)->deadline <= deadline_ms)
        pp = &(*pp)->next;
    t->next = *pp;
    *pp = t;
    t->armed = true;
    irq_restore(flags);
}

void ktimer_cancel(struct ktimer *t)
{
    uint64_t flags = irq_save();
    if (t->armed)
        unlink(t);
    irq_restore(flags);
}

void ktimer_tick(uint64_t now_ms)
{
    while (timers && timers->deadline <= now_ms) {
        struct ktimer *t = timers;
        timers = t->next;
        t->armed = false;
        t->fn(t);    // may arm it again
    }
}
