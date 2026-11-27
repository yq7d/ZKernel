#pragma once
#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port) : "memory");
    return ret;
}

/* write to an unused port, ~1us delay for slow hardware like the pic */
static inline void io_wait(void)
{
    outb(0x80, 0);
}

static inline __attribute__((noreturn)) void hcf(void)
{
    for (;;)
        __asm__ volatile ("cli; hlt");
}
