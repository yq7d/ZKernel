#pragma once
// psf1/psf2 bitmap fonts
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct font {
    uint32_t width, height;
    uint32_t glyph_count;
    uint32_t bytes_per_row;
    uint32_t bytes_per_glyph;
    const uint8_t *glyphs;
    uint16_t map[256];          // latin-1 -> glyph index
};

bool font_load_psf(struct font *f, const void *data, size_t size);

const uint8_t *font_glyph(const struct font *f, unsigned char c);

// font.asm
extern const uint8_t font_psf_start[], font_psf_end[];
