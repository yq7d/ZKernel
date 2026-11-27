#include "gfx.h"

static int rs = 16, gs = 8, bs = 0;

void gfx_set_format(const struct zk_fbinfo *info)
{
    rs = info->red_shift;
    gs = info->green_shift;
    bs = info->blue_shift;
}

uint32_t gfx_color(uint32_t rgb)
{
    return ((rgb >> 16 & 0xFF) << rs) | ((rgb >> 8 & 0xFF) << gs) | ((rgb & 0xFF) << bs);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, int t)
{
    uint32_t out = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int x = (int)(a >> sh & 0xFF), y = (int)(b >> sh & 0xFF);
        out |= (uint32_t)(x + (y - x) * t / 255) << sh;
    }
    return out;
}

void gfx_fill(struct canvas *c, int x, int y, int w, int h, uint32_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > c->w) w = c->w - x;
    if (y + h > c->h) h = c->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int row = y; row < y + h; row++) {
        uint32_t *p = c->px + (size_t)row * c->stride + x;
        for (int i = 0; i < w; i++)
            p[i] = color;
    }
}

void gfx_frame(struct canvas *c, int x, int y, int w, int h, int t, uint32_t color)
{
    gfx_fill(c, x, y, w, t, color);
    gfx_fill(c, x, y + h - t, w, t, color);
    gfx_fill(c, x, y, t, h, color);
    gfx_fill(c, x + w - t, y, t, h, color);
}

void gfx_glyph(struct canvas *c, const struct font *f, int x, int y, unsigned char ch,
               uint32_t fg, uint32_t bg, int scale)
{
    const uint8_t *g = font_glyph(f, ch);
    int gw = (int)f->width, gh = (int)f->height;
    for (int row = 0; row < gh; row++) {
        const uint8_t *bits = g + (size_t)row * f->bytes_per_row;
        for (int sy = 0; sy < scale; sy++) {
            int py = y + row * scale + sy;
            if (py < 0 || py >= c->h)
                continue;
            uint32_t *line = c->px + (size_t)py * c->stride;
            for (int col = 0; col < gw; col++) {
                bool on = bits[col / 8] & (0x80 >> (col % 8));
                if (!on && bg == GFX_CLEAR)
                    continue;
                for (int sx = 0; sx < scale; sx++) {
                    int px = x + col * scale + sx;
                    if (px >= 0 && px < c->w)
                        line[px] = on ? fg : bg;
                }
            }
        }
    }
}

int gfx_text(struct canvas *c, const struct font *f, int x, int y, const char *s,
             uint32_t fg, uint32_t bg, int scale)
{
    int start = x;
    for (; *s; s++) {
        gfx_glyph(c, f, x, y, (unsigned char)*s, fg, bg, scale);
        x += (int)f->width * scale;
    }
    return x - start;
}

bool gfx_load_font(struct font *f, const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    struct zk_stat st;
    if (fstat(fd, &st) < 0 || st.size == 0 || st.size > (1 << 20)) {
        close(fd);
        return false;
    }
    uint8_t *data = malloc(st.size);
    size_t got = 0;
    while (data && got < st.size) {
        long n = read(fd, data + got, st.size - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (!data || got != st.size || !font_load_psf(f, data, got)) {
        free(data);
        return false;
    }
    return true;
}
