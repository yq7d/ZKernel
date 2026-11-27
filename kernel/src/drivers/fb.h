#pragma once
#include <stdbool.h>
#include <stdint.h>

struct limine_framebuffer;

struct framebuffer {
    uint8_t *addr;
    uint32_t width, height;
    uint32_t pitch;
    uint8_t  red_shift, green_shift, blue_shift;
};

extern struct framebuffer fb;

bool     fb_init(struct limine_framebuffer *lfb);
uint32_t fb_rgb(uint32_t rgb);
void     fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void     fb_clear(uint32_t color);
void     fb_scroll(uint32_t rows, uint32_t color);

static inline void fb_putpixel(uint32_t x, uint32_t y, uint32_t color)
{
    if (x < fb.width && y < fb.height)
        ((uint32_t *)(fb.addr + y * fb.pitch))[x] = color;
}
