// gcc can emit calls to the mem* functions even with -ffreestanding
#include "lib/string.h"
#include <stdint.h>

void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    void *ret = dst;
    __asm__ volatile ("rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n) : : "memory");
    return ret;
}

void *memset(void *dst, int c, size_t n)
{
    void *ret = dst;
    __asm__ volatile ("rep stosb"
                      : "+D"(dst), "+c"(n) : "a"(c) : "memory");
    return ret;
}

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);

    // overlapping, copy backwards
    d += n - 1;
    s += n - 1;
    __asm__ volatile ("std; rep movsb; cld"
                      : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a, *y = b;
    for (; n; n--, x++, y++)
        if (*x != *y)
            return *x < *y ? -1 : 1;
    return 0;
}

size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return (size_t)(p - s);
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (!*a)
            return 0;
    }
    return 0;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return NULL;
    }
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    for (;; s++) {
        if (*s == (char)c)
            last = s;
        if (!*s)
            return (char *)last;
    }
}

size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size) {
        size_t n = len < size - 1 ? len : size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
    }
    return len;
}

size_t strlcat(char *dst, const char *src, size_t size)
{
    size_t dlen = strnlen(dst, size);
    if (dlen == size)
        return size + strlen(src);
    return dlen + strlcpy(dst + dlen, src, size - dlen);
}
