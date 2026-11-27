#pragma once
/* framebuffer text console. supports a small subset of ansi escapes (SGR colors, 2J, H) */
#include <stdbool.h>
#include <stddef.h>

bool console_init(void);
bool console_ready(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_clear(void);
