#pragma once
// tiny libc for zkernel user programs
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>
#include "lib/string.h"

__attribute__((noreturn)) void exit(int code);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);
long getpid(void);
void sleep_ms(uint64_t ms);
void yield(void);
uint64_t uptime_ms(void);
void *alloc_pages(size_t count);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int puts(const char *s);
int getchar(void);
// reads a line with echo and backspace, without the newline. returns length
size_t readline(char *buf, size_t size);

int atoi(const char *s);

// bump allocator on top of alloc_pages, free does nothing yet
void *malloc(size_t size);
void free(void *p);
