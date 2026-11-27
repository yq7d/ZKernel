#pragma once
/* %d %i %u %x %X %o %b %p %c %s %%, flags -0+ #, width/precision, hh h l ll z t j */
#include <stdarg.h>
#include <stddef.h>

typedef void (*putc_fn)(char c, void *ctx);

int kvformat(putc_fn out, void *ctx, const char *fmt, va_list ap);

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);
int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
