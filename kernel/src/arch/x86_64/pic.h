#pragma once
#include <stdbool.h>
#include <stdint.h>

void pic_init(uint8_t vector_base);
void pic_mask(uint8_t irq);
void pic_unmask(uint8_t irq);
void pic_eoi(uint8_t irq);
bool pic_is_spurious(uint8_t irq);
