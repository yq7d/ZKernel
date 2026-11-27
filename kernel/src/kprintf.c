#include "kprintf.h"
#include "console/console.h"
#include "drivers/serial.h"
#include "lib/printf.h"
#include "lib/spinlock.h"

#define LOG_SIZE (64 * 1024)

// keeps whole messages together
static spinlock_t lock = SPINLOCK_INIT;

// everything printed, for dmesg. colour escapes are left out
static char log_buf[LOG_SIZE];
static uint64_t log_head;
static bool in_escape;

static void log_putc(char c)
{
    if (c == '\e') {
        in_escape = true;
        return;
    }
    if (in_escape) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
            in_escape = false;
        return;
    }
    log_buf[log_head++ % LOG_SIZE] = c;
}

static void kputc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
    console_putc(c);
    log_putc(c);
}

static void raw_putc(char c, void *ctx)
{
    (void)ctx;
    serial_putc(c);
    console_putc(c);
}

void console_out(const char *s, size_t n)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    for (size_t i = 0; i < n; i++)
        raw_putc(s[i], NULL);
    spin_unlock_irqrestore(&lock, flags);
}

size_t klog_read(char *out, size_t size)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint64_t start = log_head > LOG_SIZE ? log_head - LOG_SIZE : 0;
    if (log_head - start > size)
        start = log_head - size;
    size_t n = 0;
    for (uint64_t i = start; i < log_head; i++)
        out[n++] = log_buf[i % LOG_SIZE];
    spin_unlock_irqrestore(&lock, flags);
    return n;
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
