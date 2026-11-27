#include "lib/random.h"
#include <stdbool.h>
#include <stdint.h>

static uint64_t state;

static bool have_rdrand(void)
{
    static int known = -1;
    if (known < 0) {
        uint32_t a, b, c, d;
        __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
        known = (c >> 30) & 1;
    }
    return known;
}

static uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t next(void)
{
    if (have_rdrand()) {
        uint64_t v;
        for (int i = 0; i < 10; i++) {
            unsigned char ok;
            __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok));
            if (ok)
                return v;
        }
    }
    // splitmix64, with the tsc stirred in every time
    state += 0x9e3779b97f4a7c15UL ^ rdtsc();
    uint64_t z = state;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9UL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebUL;
    return z ^ (z >> 31);
}

void random_bytes(void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        uint64_t v = next();
        for (int i = 0; i < 8 && len; i++, len--, v >>= 8)
            *p++ = (uint8_t)v;
    }
}
