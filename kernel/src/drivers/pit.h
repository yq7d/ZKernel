#pragma once
#include <stdint.h>

#define TIMER_HZ 1000

void timer_init(uint32_t hz);
uint64_t timer_ticks(void);
uint64_t timer_uptime_ns(void);
uint64_t timer_uptime_ms(void);

// needs interrupts enabled
void timer_sleep_ms(uint64_t ms);
