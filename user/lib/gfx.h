#pragma once
// drawing into 32 bit pixel buffers, plus psf fonts
#include "zk.h"
#include "lib/font.h"

struct canvas {
    uint32_t *px;
    int w, h;
    int stride;    // pixels per row
};

// colors are 0xRRGGBB, gfx_color turns them into the framebuffer's format
void gfx_set_format(const struct zk_fbinfo *info);
uint32_t gfx_color(uint32_t rgb);
// blend two 0xRRGGBB colors, t from 0 (a) to 255 (b)
uint32_t gfx_mix(uint32_t a, uint32_t b, int t);

void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t color);
void gfx_frame(struct canvas *c, int x, int y, int w, int h, int thick, uint32_t color);
// bg 0xFFFFFFFF leaves the background alone
void gfx_glyph(struct canvas *c, const struct font *f, int x, int y, unsigned char ch,
               uint32_t fg, uint32_t bg, int scale);
// returns the width drawn
int gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s,
             uint32_t fg, uint32_t bg, int scale);

#define GFX_CLEAR 0xFFFFFFFFu

// loads a psf file, the data stays allocated for the font's lifetime
bool gfx_load_font(struct font *f, const char *path);
