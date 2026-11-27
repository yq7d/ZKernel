#include "zk.h"
#include "lib/printf.h"

#define PAGE_SIZE 4096

static inline long syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "memory");
    return ret;
}

void exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}

long write(int fd, const void *buf, size_t len)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

long read(int fd, void *buf, size_t len)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

long getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

void sleep_ms(uint64_t ms)
{
    syscall3(SYS_SLEEP_MS, (long)ms, 0, 0);
}

void yield(void)
{
    syscall3(SYS_YIELD, 0, 0, 0);
}

uint64_t uptime_ms(void)
{
    return (uint64_t)syscall3(SYS_UPTIME_MS, 0, 0, 0);
}

void *alloc_pages(size_t count)
{
    long r = syscall3(SYS_ALLOC_PAGES, (long)count, 0, 0);
    return r < 0 ? NULL : (void *)r;
}

struct outbuf {
    char data[256];
    size_t len;
};

static void flush(struct outbuf *o)
{
    if (o->len)
        write(1, o->data, o->len);
    o->len = 0;
}

static void buf_putc(char c, void *ctx)
{
    struct outbuf *o = ctx;
    o->data[o->len++] = c;
    if (o->len == sizeof o->data)
        flush(o);
}

int printf(const char *fmt, ...)
{
    struct outbuf o = { .len = 0 };
    va_list ap;
    va_start(ap, fmt);
    int n = kvformat(buf_putc, &o, fmt, ap);
    va_end(ap);
    flush(&o);
    return n;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    write(1, s, len);
    write(1, "\n", 1);
    return (int)len + 1;
}

int getchar(void)
{
    char c;
    return read(0, &c, 1) == 1 ? (unsigned char)c : -1;
}

size_t readline(char *buf, size_t size)
{
    size_t len = 0;
    for (;;) {
        int c = getchar();
        if (c < 0 || c == '\n') {
            write(1, "\n", 1);
            break;
        }
        if (c == '\b') {
            if (len) {
                len--;
                write(1, "\b \b", 3);
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len + 1 < size) {
            buf[len++] = (char)c;
            write(1, &buf[len - 1], 1);
        }
    }
    buf[len] = '\0';
    return len;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}

static uint8_t *heap_cur, *heap_end;

void *malloc(size_t size)
{
    size = (size + 15) & ~(size_t)15;
    if (!size)
        return NULL;
    if ((size_t)(heap_end - heap_cur) < size) {
        size_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
        if (pages < 16)
            pages = 16;
        uint8_t *p = alloc_pages(pages);
        if (!p)
            return NULL;
        // fresh pages follow the old ones, so the old tail can still be used
        if (p != heap_end)
            heap_cur = p;
        heap_end = p + pages * PAGE_SIZE;
    }
    void *r = heap_cur;
    heap_cur += size;
    return r;
}

void free(void *p)
{
    (void)p;
}
