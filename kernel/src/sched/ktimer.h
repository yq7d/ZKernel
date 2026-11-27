#pragma once
// one shot kernel timers. the callback runs in the timer interrupt, so it
// mustn't block: waking threads and queueing signals is fine
#include <stdbool.h>
#include <stdint.h>

struct ktimer {
    uint64_t deadline;          // timer_uptime_ms() it fires at
    void (*fn)(struct ktimer *t);
    bool armed;
    struct ktimer *next;
};

// (re)arms t, replacing an earlier deadline
void ktimer_arm(struct ktimer *t, uint64_t deadline_ms);
void ktimer_cancel(struct ktimer *t);
// from the tick
void ktimer_tick(uint64_t now_ms);
