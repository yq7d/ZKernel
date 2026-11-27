#include "kprintf.h"
#include "console/console.h"
#include "drivers/serial.h"
#include "lib/printf.h"

static void kputc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
    console_putc(c);
}

int kvprintf(const char *fmt, va_list ap)
{
    return kvformat(kputc, NULL, fmt, ap);
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
