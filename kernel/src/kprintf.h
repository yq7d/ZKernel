#pragma once
#include <stdarg.h>
#include "panic.h"

int kprintf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int kvprintf(const char *fmt, va_list ap);

#define pr_info(fmt, ...)  kprintf("\e[36m[ info ]\e[0m " fmt, ##__VA_ARGS__)
#define pr_ok(fmt, ...)    kprintf("\e[32m[  ok  ]\e[0m " fmt, ##__VA_ARGS__)
#define pr_warn(fmt, ...)  kprintf("\e[33m[ warn ]\e[0m " fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)   kprintf("\e[31m[error ]\e[0m " fmt, ##__VA_ARGS__)
