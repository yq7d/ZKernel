#include "console/console.h"
#include "console/font.h"
#include "drivers/fb.h"

static const uint32_t palette[16] = {
    0x1d1f21, 0xcc6666, 0x8cb867, 0xe0b05a, 0x5f8fd0, 0xb07fcf, 0x5fb3b3, 0xc5c8c6,
    0x555753, 0xff7b7b, 0xb5e48c, 0xffd580, 0x81a2e8, 0xd7a6ff, 0x8be9e9, 0xffffff,
};
#define DEFAULT_FG 7
#define DEFAULT_BG 0

#define MAX_PARAMS 8

static struct {
    bool ready;
    struct font font;
    uint32_t cols, rows;
    uint32_t cx, cy;
    uint8_t fg_idx, bg_idx;
    bool bright;
    uint32_t fg, bg;

    enum { ST_NORMAL, ST_ESC, ST_CSI } state;
    uint32_t params[MAX_PARAMS];
    uint32_t nparams;
} con;

static void update_colors(void)
{
    uint8_t fg = con.fg_idx;
    if (con.bright && fg < 8)
        fg += 8;
    con.fg = fb_rgb(palette[fg]);
    con.bg = fb_rgb(palette[con.bg_idx]);
}

bool console_init(void)
{
    if (!font_load_psf(&con.font, font_psf_start,
                       (size_t)(font_psf_end - font_psf_start)))
        return false;

    con.cols = fb.width / con.font.width;
    con.rows = fb.height / con.font.height;
    con.cx = con.cy = 0;
    con.fg_idx = DEFAULT_FG;
    con.bg_idx = DEFAULT_BG;
    con.bright = false;
    con.state = ST_NORMAL;
    update_colors();
    con.ready = true;
    console_clear();
    return true;
}

bool console_ready(void)
{
    return con.ready;
}

void console_clear(void)
{
    fb_clear(con.bg);
    con.cx = con.cy = 0;
}

static void draw_char(uint32_t col, uint32_t row, unsigned char c)
{
    const struct font *f = &con.font;
    const uint8_t *glyph = font_glyph(f, c);
    uint32_t px = col * f->width;
    uint32_t py = row * f->height;

    for (uint32_t y = 0; y < f->height; y++) {
        uint32_t *line = (uint32_t *)(fb.addr + (size_t)(py + y) * fb.pitch) + px;
        const uint8_t *bits = glyph + y * f->bytes_per_row;
        for (uint32_t x = 0; x < f->width; x++)
            line[x] = (bits[x / 8] & (0x80 >> (x % 8))) ? con.fg : con.bg;
    }
}

static void newline(void)
{
    con.cx = 0;
    if (++con.cy >= con.rows) {
        fb_scroll(con.font.height, con.bg);
        con.cy = con.rows - 1;
    }
}

static void apply_sgr(void)
{
    if (con.nparams == 0)
        con.params[con.nparams++] = 0;

    for (uint32_t i = 0; i < con.nparams; i++) {
        uint32_t p = con.params[i];
        if (p == 0) {
            con.fg_idx = DEFAULT_FG;
            con.bg_idx = DEFAULT_BG;
            con.bright = false;
        } else if (p == 1) {
            con.bright = true;
        } else if (p == 22) {
            con.bright = false;
        } else if (p >= 30 && p <= 37) {
            con.fg_idx = (uint8_t)(p - 30);
        } else if (p == 39) {
            con.fg_idx = DEFAULT_FG;
        } else if (p >= 40 && p <= 47) {
            con.bg_idx = (uint8_t)(p - 40);
        } else if (p == 49) {
            con.bg_idx = DEFAULT_BG;
        } else if (p >= 90 && p <= 97) {
            con.fg_idx = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            con.bg_idx = (uint8_t)(p - 100 + 8);
        }
    }
    update_colors();
}

static void handle_csi(char final)
{
    switch (final) {
    case 'm':
        apply_sgr();
        break;
    case 'J':
        if (con.nparams && con.params[0] == 2)
            console_clear();
        break;
    case 'H': {
        uint32_t r = con.nparams > 0 && con.params[0] ? con.params[0] - 1 : 0;
        uint32_t c = con.nparams > 1 && con.params[1] ? con.params[1] - 1 : 0;
        con.cy = r < con.rows ? r : con.rows - 1;
        con.cx = c < con.cols ? c : con.cols - 1;
        break;
    }
    default:
        break;
    }
}

void console_putc(char ch)
{
    if (!con.ready)
        return;

    unsigned char c = (unsigned char)ch;

    switch (con.state) {
    case ST_ESC:
        if (c == '[') {
            con.state = ST_CSI;
            con.nparams = 0;
            con.params[0] = 0;
        } else {
            con.state = ST_NORMAL;
        }
        return;

    case ST_CSI:
        if (c >= '0' && c <= '9') {
            if (con.nparams == 0)
                con.nparams = 1;
            con.params[con.nparams - 1] = con.params[con.nparams - 1] * 10 + (c - '0');
        } else if (c == ';') {
            if (con.nparams == 0)
                con.nparams = 1;
            if (con.nparams < MAX_PARAMS)
                con.params[con.nparams++] = 0;
        } else if (c >= 0x40 && c <= 0x7E) {
            handle_csi((char)c);
            con.state = ST_NORMAL;
        }
        return;

    case ST_NORMAL:
        break;
    }

    switch (c) {
    case '\x1b':
        con.state = ST_ESC;
        return;
    case '\n':
        newline();
        return;
    case '\r':
        con.cx = 0;
        return;
    case '\t':
        do {
            console_putc(' ');
        } while (con.cx % 8);
        return;
    case '\b':
        if (con.cx > 0)
            con.cx--;
        return;
    }

    if (c < 0x20)
        return;

    if (con.cx >= con.cols)
        newline();
    draw_char(con.cx++, con.cy, c);
}

void console_write(const char *s, size_t n)
{
    while (n--)
        console_putc(*s++);
}
