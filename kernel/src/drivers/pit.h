#pragma once
#include <stdint.h>

#define TIMER_HZ 1000

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ns(void);
uint64_t timer_uptime_ms(void);

// blocks the thread once the scheduler runs, before that it halts
void timer_sleep_ms(uint64_t ms);
