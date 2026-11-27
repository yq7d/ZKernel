#pragma once
#include <stdbool.h>

bool serial_init(void);
void serial_putc(char c);
void serial_puts(const char *s);
void serial_puthex(unsigned long v);
void serial_putdec(unsigned long v);
