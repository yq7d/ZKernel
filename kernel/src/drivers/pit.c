// 8253/8254 pit, channel 0 on irq 0
#include "drivers/pit.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"

#define PIT_CH0      0x40
#define PIT_CMD      0x43
#define PIT_BASE_HZ  1193182UL
#define PIT_MODE2    0x34    // channel 0, lo/hi byte, rate generator

static volatile uint64_t ticks;
static uint64_t ns_per_tick;

static void timer_irq(struct interrupt_frame *frame)
{
    (void)frame;
    ticks++;
}

void timer_init(uint32_t hz)
{
    uint32_t divisor = (uint32_t)(PIT_BASE_HZ / hz);
    if (divisor == 0)
        divisor = 1;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;
    ns_per_tick = (uint64_t)divisor * 1000000000UL / PIT_BASE_HZ;

    outb(PIT_CMD, PIT_MODE2);
    outb(PIT_CH0, divisor & 0xFF);
    outb(PIT_CH0, divisor >> 8);

    irq_set_handler(0, timer_irq);
}

uint64_t timer_ticks(void)
{
    return ticks;
}

uint64_t timer_uptime_ns(void)
{
    return ticks * ns_per_tick;
}

uint64_t timer_uptime_ms(void)
{
    return timer_uptime_ns() / 1000000;
}

void timer_sleep_ms(uint64_t ms)
{
    uint64_t target = timer_uptime_ns() + ms * 1000000;
    while (timer_uptime_ns() < target)
        __asm__ volatile ("hlt");
}
