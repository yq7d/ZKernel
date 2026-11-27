#pragma once
#include <stddef.h>

// rdrand when the cpu has it, otherwise a generator seeded from the tsc.
// fine for salts and stack canaries, not for keys
void random_bytes(void *buf, size_t len);
