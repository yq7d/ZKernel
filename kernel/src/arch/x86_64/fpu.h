#pragma once
// x87/sse state. the kernel itself never touches it, so it only needs
// saving when threads switch
#include <stdint.h>

struct fpu_state {
    uint8_t data[512];
} __attribute__((aligned(16)));

void fpu_init(void);
void fpu_init_state(struct fpu_state *s);
void fpu_save(struct fpu_state *s);
void fpu_restore(const struct fpu_state *s);
