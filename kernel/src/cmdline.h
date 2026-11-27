#pragma once
#include <stdbool.h>
#include <stddef.h>

void cmdline_init(const char *cmdline);
const char *cmdline_raw(void);

/* "key=value" -> value, bare "key" -> "" */
bool cmdline_get(const char *key, char *out, size_t size);
