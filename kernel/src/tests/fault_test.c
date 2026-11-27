/* make run KCMDLINE="fault=pf" */
#include "tests/tests.h"
#include "kprintf.h"
#include "lib/string.h"
#include <stdint.h>

static volatile uintptr_t bad_addr;
static volatile int zero;

#pragma GCC diagnostic ignored "-Winfinite-recursion"
static int recurse(int n)
{
    volatile char pad[4096];
    pad[0] = (char)n;
    return recurse(n + 1) + pad[0];
}

void trigger_fault(const char *name)
{
    pr_warn("fault test: triggering '%s'\n", name);

    if (strcmp(name, "de") == 0) {
        /* gcc turns 1/x into compares, so use div directly */
        uint32_t divisor = (uint32_t)zero;
        __asm__ volatile ("xor %%edx, %%edx; mov $1, %%eax; div %0"
                          : : "r"(divisor) : "eax", "edx");
    } else if (strcmp(name, "ud") == 0) {
        __asm__ volatile ("ud2");
    } else if (strcmp(name, "bp") == 0) {
        __asm__ volatile ("int3");
        pr_ok("fault test: returned from breakpoint handler\n");
        return;
    } else if (strcmp(name, "gp") == 0) {
        bad_addr = 0xdead000000000000UL;
        *(volatile uint64_t *)bad_addr = 1;
    } else if (strcmp(name, "pf") == 0) {
        bad_addr = 0x1000;
        *(volatile uint64_t *)bad_addr = 1;
    } else if (strcmp(name, "df") == 0) {
        /* bad stack, so delivering the exception faults again */
        __asm__ volatile ("mov $0x1000, %%rsp; int3" ::: "memory");
    } else if (strcmp(name, "stack") == 0) {
        /* TODO: no guard page yet, this tramples memory until it hits something unmapped */
        recurse(0);
    } else if (strcmp(name, "panic") == 0) {
        panic("fault test: explicit panic");
    } else {
        pr_err("fault test: unknown fault '%s' (try de ud bp gp pf df stack panic)\n", name);
        return;
    }

    pr_err("fault test: '%s' did not fault!\n", name);
}
