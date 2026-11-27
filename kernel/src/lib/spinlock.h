#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "arch/x86_64/cpu.h"

#define RFLAGS_IF (1UL << 9)

typedef struct {
    volatile bool locked;
} spinlock_t;

#define SPINLOCK_INIT { false }

// interrupts stay off while held, so an irq handler can never spin on a
// lock its own cpu already holds
static inline uint64_t spin_lock_irqsave(spinlock_t *l)
{
    uint64_t flags = read_rflags();
    cli();
    while (__atomic_test_and_set(&l->locked, __ATOMIC_ACQUIRE))
        __asm__ volatile ("pause");
    return flags;
}

static inline void spin_unlock_irqrestore(spinlock_t *l, uint64_t flags)
{
    __atomic_clear(&l->locked, __ATOMIC_RELEASE);
    if (flags & RFLAGS_IF)
        sti();
}
