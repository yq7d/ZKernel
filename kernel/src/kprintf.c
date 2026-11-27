#include "kprintf.h"
#include "console/console.h"
#include "drivers/serial.h"
#include "lib/printf.h"
#include "lib/spinlock.h"

// keeps whole messages together
static spinlock_t lock = SPINLOCK_INIT;

static void kputc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
    console_putc(c);
}

int kvprintf(const char *fmt, va_list ap)
{
    if (panic_in_progress())
        return kvformat(kputc, NULL, fmt, ap);

    uint64_t flags = spin_lock_irqsave(&lock);
    int n = kvformat(kputc, NULL, fmt, ap);
    spin_unlock_irqrestore(&lock, flags);
    return n;
}

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = kvprintf(fmt, ap);
    va_end(ap);
    return n;
}
