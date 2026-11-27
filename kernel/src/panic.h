#pragma once
#include <stdint.h>

__attribute__((noreturn, format(printf, 3, 4)))
void panic_at(const char *file, int line, const char *fmt, ...);

#define panic(...) panic_at(__FILE__, __LINE__, __VA_ARGS__)

#define kassert(expr) \
    do { if (!(expr)) panic("assertion failed: %s", #expr); } while (0)

/* used by the exception handler too */
void panic_begin(void);
void print_backtrace(uint64_t rbp);
