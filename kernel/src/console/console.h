#pragma once
// framebuffer text console. supports a small subset of ansi escapes (SGR colors, 2J, H)
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool console_init(void);
bool console_ready(void);
void console_putc(char c);
void console_write(const char *s, size_t n);
void console_clear(void);
void console_cursor_show(void);
void console_cursor_hide(void);
uint32_t console_cols(void);
uint32_t console_rows(void);
// stops drawing while a program has the framebuffer, clears it on resume
void console_suspend(bool on);
