#include "drivers/fb.h"
#include "lib/string.h"
#include <limine.h>

struct framebuffer fb;

bool fb_init(struct limine_framebuffer *lfb)
{
    /* only 32bpp rgb for now */
    if (lfb->bpp != 32 || lfb->memory_model != LIMINE_FRAMEBUFFER_RGB)
        return false;

    fb.addr        = lfb->address;
    fb.width       = (uint32_t)lfb->width;
    fb.height      = (uint32_t)lfb->height;
    fb.pitch       = (uint32_t)lfb->pitch;
    fb.red_shift   = lfb->red_mask_shift;
    fb.green_shift = lfb->green_mask_shift;
    fb.blue_shift  = lfb->blue_mask_shift;
    return true;
}

uint32_t fb_rgb(uint32_t rgb)
{
    return (((rgb >> 16) & 0xFF) << fb.red_shift) |
           (((rgb >> 8)  & 0xFF) << fb.green_shift) |
           ((rgb         & 0xFF) << fb.blue_shift);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color)
{
    if (x >= fb.width || y >= fb.height)
        return;
    if (w > fb.width - x)
        w = fb.width - x;
    if (h > fb.height - y)
        h = fb.height - y;

    for (uint32_t row = y; row < y + h; row++) {
        uint32_t *p = (uint32_t *)(fb.addr + (size_t)row * fb.pitch) + x;
        for (uint32_t i = 0; i < w; i++)
            p[i] = color;
    }
}

void fb_clear(uint32_t color)
{
    fb_fill_rect(0, 0, fb.width, fb.height, color);
}

void fb_scroll(uint32_t rows, uint32_t color)
{
    if (rows >= fb.height) {
        fb_clear(color);
        return;
    }
    memmove(fb.addr, fb.addr + (size_t)rows * fb.pitch,
            (size_t)(fb.height - rows) * fb.pitch);
    fb_fill_rect(0, fb.height - rows, fb.width, rows, color);
}
