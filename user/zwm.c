// zwm, the ZOS window manager. tiling, every window is a terminal
//
//   super+enter         new terminal
//   super+q             close window
//   super+j/k, arrows   focus next/previous
//   super+shift+j/k     move window down/up the stack
//   super+shift+enter   swap with the master window
//   super+h/l           shrink/grow the master area
//   super+f             fullscreen toggle
//   super+1..9          switch workspace, with shift moves the window there
//   super+shift+r       reload the config
//   super+shift+e       quit
//
// x programs (firefox, xrun prog) show up in an "x display" window: an
// xvfb screen zwm draws in and types and clicks into through xtest
//
// settings come from /etc/zwm.conf, then ~/.config/zwm/zwm.conf on top
//
// alt works as the modifier too, host desktops often keep super for themselves
#include "zk.h"
#include "gfx.h"
#include "lib/time.h"

#define MAX_TERMS  32
#define NWS        9
#define BAR_H      22
#define TITLE_H    20
#define PAD        4
#define READ_CHUNK 4096

#define MAX_BINDS     32
#define MAX_AUTOSTART 8

enum { BG_SOLID, BG_GRADIENT, BG_IMAGE };
enum { FIT_FILL, FIT_CENTER, FIT_TILE };

struct bind {
    uint32_t mods;
    uint16_t code;
    char cmd[128];
};

// everything zwm.conf can change, with the built in look as defaults
static struct config {
    uint32_t desktop, desktop2, bar, bar_text, dim, accent, inactive, title_bg;
    uint32_t term_bg, term_fg, cursor;
    uint32_t palette[16];
    int gap, border, master;
    uint32_t mods;
    int bg_kind, fit;
    char wallpaper[ZK_PATH_MAX];
    char font[ZK_PATH_MAX];
    char logo[32];
    char shell[ZK_PATH_MAX];
    bool clock_24h, show_memory;
    struct bind binds[MAX_BINDS];
    int nbinds;
    char autostart[MAX_AUTOSTART][128];
    int nautostart;
} cfg;

static const struct config defaults = {
    .desktop = 0x121620, .desktop2 = 0x0b0d11, .bar = 0x0b0d11, .bar_text = 0xc9d1dc,
    .dim = 0x5c6573, .accent = 0x5fa8d3, .inactive = 0x2a2f38, .title_bg = 0x161a20,
    .term_bg = 0x14171c, .term_fg = 0xd0d6de, .cursor = 0x5fa8d3,
    .palette = {
        0x1c1f24, 0xe06c75, 0x98c379, 0xe5c07b, 0x61afef, 0xc678dd, 0x56b6c2, 0xc8ccd4,
        0x5c6370, 0xff7b86, 0xb5e48c, 0xffd580, 0x81b5ff, 0xdc9cf0, 0x8be9e9, 0xffffff,
    },
    .gap = 6, .border = 2, .master = 55,
    .mods = KEYMOD_SUPER | KEYMOD_ALT,
    .bg_kind = BG_GRADIENT, .fit = FIT_FILL,
    .font = "/usr/share/fonts/ter-v16n.psf",
    .logo = "ZOS",
    .shell = "/bin/sh",
    .clock_24h = true, .show_memory = true,
};

// the wallpaper image scaled to the screen, in screen pixel format
static uint32_t *wallpaper;
#define FG_DEFAULT 16
#define BG_DEFAULT 17

#define ATTR_BOLD    1
#define ATTR_REVERSE 2

struct cell {
    uint8_t ch, fg, bg, attr;
};

enum { S_NORMAL, S_ESC, S_CSI, S_OSC };

struct term {
    bool used;
    int master;
    long pid;
    int ws;
    char title[32];
    char osc_title[32];
    int x, y, w, h;          // whole window
    int cols, rows;
    struct cell *cells;
    int cx, cy, saved_x, saved_y;
    uint8_t fg, bg, attr;
    bool cursor_on, wrap_next;
    int state;
    int params[8], nparams;
    bool csi_private;
    char osc[64];
    int osc_len;
    bool dirty;
    bool xwin;               // the x display, not a terminal
};

static struct term terms[MAX_TERMS];
static int order[MAX_TERMS];    // stacking order, indexes into terms
static int nterms;
static int cur_ws;
static int focus = -1;          // terms index
static int fullscreen = -1;
static int mfact[NWS];          // master width in percent

static struct canvas screen;    // back buffer
static uint32_t *fbmem;
static struct zk_fbinfo fbi;
static struct font font;
static int fw, fh;
static bool full_redraw = true, bar_dirty = true;
static int mouse_x, mouse_y, old_mx = -100, old_my = -100;
static uint8_t mouse_buttons;
static bool pointer_moved;

static uint32_t col(uint32_t rgb)
{
    return gfx_color(rgb);
}

// terminal emulation

static struct cell blank(struct term *t)
{
    return (struct cell){ ' ', t->fg, t->bg, 0 };
}

static struct cell *at(struct term *t, int x, int y)
{
    return &t->cells[y * t->cols + x];
}

static void clear_cells(struct term *t, int from, int to)
{
    struct cell b = blank(t);
    for (int i = from; i < to; i++)
        t->cells[i] = b;
    t->dirty = true;
}

static void scroll_up(struct term *t, int n)
{
    if (n >= t->rows) {
        clear_cells(t, 0, t->rows * t->cols);
        return;
    }
    memmove(t->cells, t->cells + n * t->cols,
            (size_t)(t->rows - n) * t->cols * sizeof(struct cell));
    clear_cells(t, (t->rows - n) * t->cols, t->rows * t->cols);
}

static void scroll_down(struct term *t, int top, int n)
{
    if (n > t->rows - top)
        n = t->rows - top;
    memmove(t->cells + (top + n) * t->cols, t->cells + top * t->cols,
            (size_t)(t->rows - top - n) * t->cols * sizeof(struct cell));
    clear_cells(t, top * t->cols, (top + n) * t->cols);
}

static void newline(struct term *t)
{
    t->cx = 0;
    t->wrap_next = false;
    if (++t->cy >= t->rows) {
        t->cy = t->rows - 1;
        scroll_up(t, 1);
    }
}

static void clamp_cursor(struct term *t)
{
    if (t->cx < 0) t->cx = 0;
    if (t->cy < 0) t->cy = 0;
    if (t->cx >= t->cols) t->cx = t->cols - 1;
    if (t->cy >= t->rows) t->cy = t->rows - 1;
}

static void sgr(struct term *t)
{
    if (t->nparams == 0)
        t->params[t->nparams++] = 0;
    for (int i = 0; i < t->nparams; i++) {
        int p = t->params[i];
        if (p == 0) {
            t->fg = FG_DEFAULT;
            t->bg = BG_DEFAULT;
            t->attr = 0;
        } else if (p == 1) {
            t->attr |= ATTR_BOLD;
        } else if (p == 22) {
            t->attr &= ~ATTR_BOLD;
        } else if (p == 7) {
            t->attr |= ATTR_REVERSE;
        } else if (p == 27) {
            t->attr &= ~ATTR_REVERSE;
        } else if (p >= 30 && p <= 37) {
            t->fg = (uint8_t)(p - 30);
        } else if (p == 39) {
            t->fg = FG_DEFAULT;
        } else if (p >= 40 && p <= 47) {
            t->bg = (uint8_t)(p - 40);
        } else if (p == 49) {
            t->bg = BG_DEFAULT;
        } else if (p >= 90 && p <= 97) {
            t->fg = (uint8_t)(p - 90 + 8);
        } else if (p >= 100 && p <= 107) {
            t->bg = (uint8_t)(p - 100 + 8);
        }
    }
}

static int param(struct term *t, int i, int def)
{
    return i < t->nparams && t->params[i] ? t->params[i] : def;
}

static void csi(struct term *t, char final)
{
    int n = param(t, 0, 1);
    switch (final) {
    case 'm': sgr(t); break;
    case 'A': t->cy -= n; break;
    case 'B': t->cy += n; break;
    case 'C': t->cx += n; break;
    case 'D': t->cx -= n; break;
    case 'E': t->cy += n; t->cx = 0; break;
    case 'F': t->cy -= n; t->cx = 0; break;
    case 'G': t->cx = n - 1; break;
    case 'd': t->cy = n - 1; break;
    case 'H':
    case 'f':
        t->cy = param(t, 0, 1) - 1;
        t->cx = param(t, 1, 1) - 1;
        break;
    case 'J': {
        int mode = t->nparams ? t->params[0] : 0;
        int pos = t->cy * t->cols + t->cx;
        if (mode == 0)
            clear_cells(t, pos, t->rows * t->cols);
        else if (mode == 1)
            clear_cells(t, 0, pos + 1);
        else
            clear_cells(t, 0, t->rows * t->cols);
        break;
    }
    case 'K': {
        int mode = t->nparams ? t->params[0] : 0;
        int row = t->cy * t->cols;
        if (mode == 0)
            clear_cells(t, row + t->cx, row + t->cols);
        else if (mode == 1)
            clear_cells(t, row, row + t->cx + 1);
        else
            clear_cells(t, row, row + t->cols);
        break;
    }
    case 'L': scroll_down(t, t->cy, n); break;
    case 'M':
        if (t->cy == 0) {
            scroll_up(t, n);
        } else {
            for (int k = 0; k < n; k++) {
                memmove(t->cells + t->cy * t->cols, t->cells + (t->cy + 1) * t->cols,
                        (size_t)(t->rows - t->cy - 1) * t->cols * sizeof(struct cell));
                clear_cells(t, (t->rows - 1) * t->cols, t->rows * t->cols);
            }
        }
        break;
    case 'P': {
        int row = t->cy * t->cols, left = t->cols - t->cx;
        if (n > left)
            n = left;
        memmove(t->cells + row + t->cx, t->cells + row + t->cx + n,
                (size_t)(left - n) * sizeof(struct cell));
        clear_cells(t, row + t->cols - n, row + t->cols);
        break;
    }
    case '@': {
        int row = t->cy * t->cols, left = t->cols - t->cx;
        if (n > left)
            n = left;
        memmove(t->cells + row + t->cx + n, t->cells + row + t->cx,
                (size_t)(left - n) * sizeof(struct cell));
        clear_cells(t, row + t->cx, row + t->cx + n);
        break;
    }
    case 'X': {
        int row = t->cy * t->cols, end = t->cx + n;
        if (end > t->cols)
            end = t->cols;
        clear_cells(t, row + t->cx, row + end);
        break;
    }
    case 's': t->saved_x = t->cx; t->saved_y = t->cy; break;
    case 'u': t->cx = t->saved_x; t->cy = t->saved_y; break;
    case 'h':
    case 'l':
        if (t->csi_private && t->nparams && t->params[0] == 25)
            t->cursor_on = final == 'h';
        break;
    }
    t->wrap_next = false;
    clamp_cursor(t);
    t->dirty = true;
}

static void put(struct term *t, unsigned char c)
{
    if (t->wrap_next)
        newline(t);
    *at(t, t->cx, t->cy) = (struct cell){ c, t->fg, t->bg, t->attr };
    if (t->cx == t->cols - 1)
        t->wrap_next = true;
    else
        t->cx++;
    t->dirty = true;
}

static void feed(struct term *t, unsigned char c)
{
    switch (t->state) {
    case S_ESC:
        t->state = S_NORMAL;
        if (c == '[') {
            t->state = S_CSI;
            t->nparams = 0;
            t->params[0] = 0;
            t->csi_private = false;
        } else if (c == ']') {
            t->state = S_OSC;
            t->osc_len = 0;
        } else if (c == '7') {
            t->saved_x = t->cx;
            t->saved_y = t->cy;
        } else if (c == '8') {
            t->cx = t->saved_x;
            t->cy = t->saved_y;
        } else if (c == 'c') {
            t->fg = FG_DEFAULT;
            t->bg = BG_DEFAULT;
            t->attr = 0;
            t->cx = t->cy = 0;
            clear_cells(t, 0, t->rows * t->cols);
        }
        return;
    case S_CSI:
        if (c == '?') {
            t->csi_private = true;
        } else if (c >= '0' && c <= '9') {
            if (t->nparams == 0)
                t->nparams = 1;
            int *p = &t->params[t->nparams - 1];
            if (*p < 10000)
                *p = *p * 10 + (c - '0');
        } else if (c == ';') {
            if (t->nparams == 0)
                t->nparams = 1;
            if (t->nparams < 8)
                t->params[t->nparams++] = 0;
        } else if (c >= 0x40 && c <= 0x7e) {
            t->state = S_NORMAL;
            csi(t, (char)c);
        } else {
            t->state = S_NORMAL;
        }
        return;
    case S_OSC:
        // ESC ] 0 ; title BEL
        if (c == 7 || c == 0x9c) {
            t->osc[t->osc_len] = '\0';
            char *semi = strchr(t->osc, ';');
            if (semi) {
                strlcpy(t->osc_title, semi + 1, sizeof t->osc_title);
                t->dirty = true;
            }
            t->state = S_NORMAL;
        } else if (c == 0x1b) {
            t->state = S_NORMAL;
        } else if (t->osc_len + 1 < (int)sizeof t->osc) {
            t->osc[t->osc_len++] = (char)c;
        }
        return;
    }

    switch (c) {
    case 0x1b: t->state = S_ESC; break;
    case '\n': newline(t); break;
    case '\r': t->cx = 0; t->wrap_next = false; break;
    case '\b':
        if (t->cx > 0)
            t->cx--;
        t->wrap_next = false;
        break;
    case '\t':
        do
            put(t, ' ');
        while (t->cx % 8 && !t->wrap_next);
        break;
    case 7:
        break;
    default:
        if (c >= 0x20)
            put(t, c);
    }
    t->dirty = true;
}


// config

static bool parse_color(const char *v, uint32_t *out)
{
    if (*v == '#')
        v++;
    char *end;
    long c = strtol(v, &end, 16);
    if (end - v != 6)
        return false;
    *out = (uint32_t)c;
    return true;
}

static const struct {
    const char *name;
    uint16_t code;
} keynames[] = {
    { "a", 0x1e }, { "b", 0x30 }, { "c", 0x2e }, { "d", 0x20 }, { "e", 0x12 }, { "f", 0x21 },
    { "g", 0x22 }, { "h", 0x23 }, { "i", 0x17 }, { "j", 0x24 }, { "k", 0x25 }, { "l", 0x26 },
    { "m", 0x32 }, { "n", 0x31 }, { "o", 0x18 }, { "p", 0x19 }, { "q", 0x10 }, { "r", 0x13 },
    { "s", 0x1f }, { "t", 0x14 }, { "u", 0x16 }, { "v", 0x2f }, { "w", 0x11 }, { "x", 0x2d },
    { "y", 0x15 }, { "z", 0x2c }, { "0", 0x0b }, { "1", 0x02 }, { "2", 0x03 }, { "3", 0x04 },
    { "4", 0x05 }, { "5", 0x06 }, { "6", 0x07 }, { "7", 0x08 }, { "8", 0x09 }, { "9", 0x0a },
    { "enter", 0x1c }, { "space", 0x39 }, { "tab", 0x0f }, { "escape", 0x01 },
    { "backspace", 0x0e }, { "f1", 0x3b }, { "f2", 0x3c }, { "f3", 0x3d }, { "f4", 0x3e },
    { "f5", 0x3f }, { "f6", 0x40 }, { "f7", 0x41 }, { "f8", 0x42 }, { "f9", 0x43 },
    { "f10", 0x44 }, { "f11", 0x57 }, { "f12", 0x58 }, { "up", 0xc8 }, { "down", 0xd0 },
    { "left", 0xcb }, { "right", 0xcd },
};

// "super+shift+b" -> modifier bits and a scancode
static bool parse_keys(const char *spec, uint32_t *mods, uint16_t *code)
{
    char buf[64];
    strlcpy(buf, spec, sizeof buf);
    *mods = 0;
    *code = 0;
    char *save;
    for (char *k = strtok_r(buf, "+", &save); k; k = strtok_r(NULL, "+", &save)) {
        if (strcmp(k, "super") == 0) *mods |= KEYMOD_SUPER;
        else if (strcmp(k, "alt") == 0) *mods |= KEYMOD_ALT;
        else if (strcmp(k, "shift") == 0) *mods |= KEYMOD_SHIFT;
        else if (strcmp(k, "ctrl") == 0) *mods |= KEYMOD_CTRL;
        else {
            for (size_t i = 0; i < sizeof keynames / sizeof keynames[0]; i++)
                if (strcmp(k, keynames[i].name) == 0)
                    *code = keynames[i].code;
            if (!*code)
                return false;
        }
    }
    return *code != 0;
}

static void apply_setting(const char *key, char *val)
{
    uint32_t c;
    struct { const char *name; uint32_t *field; } colors[] = {
        { "accent", &cfg.accent }, { "bar", &cfg.bar }, { "bar_text", &cfg.bar_text },
        { "inactive", &cfg.inactive }, { "title", &cfg.title_bg }, { "dim", &cfg.dim },
        { "terminal_background", &cfg.term_bg }, { "terminal_foreground", &cfg.term_fg },
        { "cursor", &cfg.cursor },
    };
    for (size_t i = 0; i < sizeof colors / sizeof colors[0]; i++) {
        if (strcmp(key, colors[i].name) == 0) {
            if (parse_color(val, &c))
                *colors[i].field = c;
            return;
        }
    }
    if (strncmp(key, "color", 5) == 0 && isdigit(key[5])) {
        int n = atoi(key + 5);
        if (n >= 0 && n < 16 && parse_color(val, &c))
            cfg.palette[n] = c;
    } else if (strcmp(key, "background") == 0) {
        // "#rrggbb" or "gradient #top #bottom"
        char *save, *first = strtok_r(val, " \t", &save);
        if (first && strcmp(first, "gradient") == 0) {
            char *a = strtok_r(NULL, " \t", &save), *b = strtok_r(NULL, " \t", &save);
            if (a && b && parse_color(a, &cfg.desktop) && parse_color(b, &cfg.desktop2))
                cfg.bg_kind = BG_GRADIENT;
        } else if (first && parse_color(first, &cfg.desktop)) {
            cfg.bg_kind = BG_SOLID;
        }
    } else if (strcmp(key, "wallpaper") == 0) {
        strlcpy(cfg.wallpaper, val, sizeof cfg.wallpaper);
        cfg.bg_kind = BG_IMAGE;
    } else if (strcmp(key, "wallpaper_mode") == 0) {
        cfg.fit = strcmp(val, "center") == 0 ? FIT_CENTER : strcmp(val, "tile") == 0 ? FIT_TILE : FIT_FILL;
    } else if (strcmp(key, "gap") == 0) {
        cfg.gap = atoi(val) < 0 ? 0 : atoi(val) > 64 ? 64 : atoi(val);
    } else if (strcmp(key, "border") == 0) {
        cfg.border = atoi(val) < 0 ? 0 : atoi(val) > 16 ? 16 : atoi(val);
    } else if (strcmp(key, "master") == 0) {
        cfg.master = atoi(val) < 20 ? 20 : atoi(val) > 80 ? 80 : atoi(val);
    } else if (strcmp(key, "mod") == 0) {
        cfg.mods = 0;
        if (strstr(val, "super"))
            cfg.mods |= KEYMOD_SUPER;
        if (strstr(val, "alt"))
            cfg.mods |= KEYMOD_ALT;
        if (!cfg.mods)
            cfg.mods = KEYMOD_SUPER;
    } else if (strcmp(key, "font") == 0) {
        strlcpy(cfg.font, val, sizeof cfg.font);
    } else if (strcmp(key, "logo") == 0) {
        strlcpy(cfg.logo, val, sizeof cfg.logo);
    } else if (strcmp(key, "shell") == 0) {
        strlcpy(cfg.shell, val, sizeof cfg.shell);
    } else if (strcmp(key, "clock") == 0) {
        cfg.clock_24h = strcmp(val, "12h") != 0;
    } else if (strcmp(key, "show_memory") == 0) {
        cfg.show_memory = strcmp(val, "no") != 0 && strcmp(val, "false") != 0;
    } else if (strcmp(key, "bind") == 0 && cfg.nbinds < MAX_BINDS) {
        // bind = super+b fetch -o /tmp/x http://...
        char *sp = val;
        while (*sp && !isspace(*sp))
            sp++;
        if (!*sp)
            return;
        *sp++ = '\0';
        while (isspace(*sp))
            sp++;
        struct bind *b = &cfg.binds[cfg.nbinds];
        if (parse_keys(val, &b->mods, &b->code)) {
            strlcpy(b->cmd, sp, sizeof b->cmd);
            cfg.nbinds++;
        }
    } else if (strcmp(key, "autostart") == 0 && cfg.nautostart < MAX_AUTOSTART) {
        strlcpy(cfg.autostart[cfg.nautostart++], val, sizeof cfg.autostart[0]);
    }
}

static void read_config(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return;
    struct lreader *r = lr_open(fd);
    char *line;
    while (lr_getline(r, &line) >= 0) {
        while (isspace(*line))
            line++;
        if (!*line || *line == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        char *key = line, *val = eq + 1;
        *eq = '\0';
        char *end = eq;
        while (end > key && isspace(end[-1]))
            *--end = '\0';
        while (isspace(*val))
            val++;
        end = val + strlen(val);
        while (end > val && isspace(end[-1]))
            *--end = '\0';
        // quotes are optional
        if (*val == '"' && end > val + 1 && end[-1] == '"') {
            end[-1] = '\0';
            val++;
        }
        apply_setting(key, val);
    }
    lr_close(r);
    close(fd);
}

// a binary ppm (P6), scaled or placed per wallpaper_mode
static void load_wallpaper(void)
{
    free(wallpaper);
    wallpaper = NULL;
    if (cfg.bg_kind != BG_IMAGE)
        return;
    int fd = open(cfg.wallpaper, O_RDONLY);
    if (fd < 0) {
        cfg.bg_kind = BG_GRADIENT;
        return;
    }
    struct zk_stat st;
    fstat(fd, &st);
    uint8_t *data = malloc(st.size);
    size_t got = 0;
    long n;
    while (data && got < st.size && (n = read(fd, data + got, st.size - got)) > 0)
        got += (size_t)n;
    close(fd);
    // header: P6 <w> <h> <max>, with comments allowed
    int vals[3], nv = 0;
    size_t pos = 2;
    if (!data || got < 3 || data[0] != 'P' || data[1] != '6')
        goto bad;
    while (nv < 3 && pos < got) {
        if (data[pos] == '#') {
            while (pos < got && data[pos] != '\n')
                pos++;
        } else if (isdigit(data[pos])) {
            int v = 0;
            while (pos < got && isdigit(data[pos]))
                v = v * 10 + (data[pos++] - '0');
            vals[nv++] = v;
            continue;
        }
        pos++;
    }
    pos++;    // the single whitespace after maxval
    int iw = vals[0], ih = vals[1];
    if (nv < 3 || vals[2] != 255 || iw <= 0 || ih <= 0 || pos + (size_t)iw * ih * 3 > got)
        goto bad;
    wallpaper = malloc((size_t)screen.w * screen.h * 4);
    if (!wallpaper)
        goto bad;
    const uint8_t *px = data + pos;
    for (int y = 0; y < screen.h; y++) {
        for (int x = 0; x < screen.w; x++) {
            int sx, sy;
            if (cfg.fit == FIT_FILL) {
                sx = x * iw / screen.w;
                sy = y * ih / screen.h;
            } else if (cfg.fit == FIT_TILE) {
                sx = x % iw;
                sy = y % ih;
            } else {
                sx = x - (screen.w - iw) / 2;
                sy = y - (screen.h - ih) / 2;
            }
            uint32_t c = cfg.desktop;
            if (sx >= 0 && sy >= 0 && sx < iw && sy < ih) {
                const uint8_t *p = px + ((size_t)sy * iw + sx) * 3;
                c = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
            }
            wallpaper[(size_t)y * screen.w + x] = gfx_color(c);
        }
    }
    free(data);
    return;
bad:
    free(data);
    cfg.bg_kind = BG_GRADIENT;
}

static void load_config(void)
{
    cfg = defaults;
    read_config("/etc/zwm.conf");
    const char *home = getenv("HOME");
    if (home) {
        char path[ZK_PATH_MAX];
        snprintf(path, sizeof path, "%s/.config/zwm/zwm.conf", home);
        read_config(path);
    }
    for (int i = 0; i < NWS; i++)
        mfact[i] = cfg.master;
}

static void load_font(void)
{
    struct font f;
    if (gfx_load_font(&f, cfg.font) || gfx_load_font(&f, defaults.font)) {
        font = f;
        fw = (int)font.width;
        fh = (int)font.height;
    }
}

// windows

static bool visible(int i)
{
    return terms[i].used && terms[i].ws == cur_ws;
}

static void resize_term(struct term *t, int w, int h)
{
    t->x = 0;
    t->w = w;
    t->h = h;
    if (t->xwin)
        return;
    int cols = (w - 2 * cfg.border - 2 * PAD) / fw;
    int rows = (h - 2 * cfg.border - TITLE_H - 2 * PAD) / fh;
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    if (cols == t->cols && rows == t->rows && t->cells)
        return;

    struct cell *cells = malloc((size_t)cols * rows * sizeof(struct cell));
    if (!cells)
        return;
    struct cell b = { ' ', FG_DEFAULT, BG_DEFAULT, 0 };
    for (int i = 0; i < cols * rows; i++)
        cells[i] = b;
    // keep the bottom of the old screen, that's where the prompt is
    if (t->cells) {
        int shift = t->cy >= rows ? t->cy - rows + 1 : 0;
        for (int y = 0; y < rows && y + shift < t->rows; y++)
            for (int x = 0; x < cols && x < t->cols; x++)
                cells[y * cols + x] = t->cells[(y + shift) * t->cols + x];
        t->cy -= shift;
        free(t->cells);
    }
    t->cells = cells;
    t->cols = cols;
    t->rows = rows;
    clamp_cursor(t);
    t->wrap_next = false;

    struct zk_winsize ws = { (uint16_t)rows, (uint16_t)cols,
                             (uint16_t)(cols * fw), (uint16_t)(rows * fh) };
    ioctl(t->master, TIOCSWINSZ, (uint64_t)&ws);
}

static void place(struct term *t, int x, int y, int w, int h)
{
    resize_term(t, w, h);
    t->x = x;
    t->y = y;
    t->dirty = true;
}

static void layout(void)
{
    int vis[MAX_TERMS], n = 0;
    for (int k = 0; k < nterms; k++)
        if (visible(order[k]))
            vis[n++] = order[k];

    int ax = cfg.gap, ay = BAR_H + cfg.gap, aw = screen.w - 2 * cfg.gap, ah = screen.h - BAR_H - 2 * cfg.gap;
    if (fullscreen >= 0 && visible(fullscreen)) {
        place(&terms[fullscreen], 0, BAR_H, screen.w, screen.h - BAR_H);
        full_redraw = true;
        return;
    }
    fullscreen = -1;
    if (n == 1) {
        place(&terms[vis[0]], ax, ay, aw, ah);
    } else if (n > 1) {
        int mw = aw * mfact[cur_ws] / 100;
        place(&terms[vis[0]], ax, ay, mw - cfg.gap / 2, ah);
        int sx = ax + mw + cfg.gap / 2, sw = aw - mw - cfg.gap / 2;
        int each = (ah - cfg.gap * (n - 2)) / (n - 1);
        for (int i = 1; i < n; i++) {
            int y = ay + (i - 1) * (each + cfg.gap);
            int h = i == n - 1 ? ay + ah - y : each;
            place(&terms[vis[i]], sx, y, sw, h);
        }
    }
    full_redraw = true;
}

static int pos_in_order(int t)
{
    for (int k = 0; k < nterms; k++)
        if (order[k] == t)
            return k;
    return -1;
}

static void focus_any(void)
{
    if (focus >= 0 && visible(focus))
        return;
    focus = -1;
    for (int k = 0; k < nterms; k++) {
        if (visible(order[k])) {
            focus = order[k];
            break;
        }
    }
    bar_dirty = true;
}

// new windows become the master
static void add_window(int idx)
{
    memmove(order + 1, order, (size_t)nterms * sizeof(int));
    order[0] = idx;
    nterms++;
    focus = idx;
    fullscreen = -1;
    layout();
}

static int free_slot(void)
{
    for (int i = 0; i < MAX_TERMS; i++)
        if (!terms[i].used)
            return i;
    return -1;
}

// a terminal running cmd through the shell, which stays afterwards
static void spawn_term_cmd(const char *cmd);

static void spawn_term(void)
{
    spawn_term_cmd(NULL);
}

static void spawn_term_cmd(const char *cmd)
{
    int idx = -1;
    for (int i = 0; i < MAX_TERMS; i++) {
        if (!terms[i].used) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return;
    int pty[2];
    if (openpty(pty) < 0)
        return;

    struct term *t = &terms[idx];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->master = pty[0];
    t->ws = cur_ws;
    t->fg = FG_DEFAULT;
    t->bg = BG_DEFAULT;
    t->cursor_on = true;
    strlcpy(t->title, "sh", sizeof t->title);
    resize_term(t, 640, 400);    // real size comes from layout

    char line[192];
    snprintf(line, sizeof line, "%s; exec %s", cmd ? cmd : "", cfg.shell);
    char *argv[] = { "sh", cmd ? "-c" : NULL, line, NULL };
    struct zk_spawn opts = { { pty[1], pty[1], pty[1] }, 0 };
    t->pid = spawn(cfg.shell, argv, &opts);
    close(pty[1]);
    if (t->pid < 0) {
        close(t->master);
        free(t->cells);
        t->used = false;
        return;
    }

    add_window(idx);
}

static void remove_term(int idx)
{
    struct term *t = &terms[idx];
    if (!t->used)
        return;
    if (t->master >= 0)
        close(t->master);
    free(t->cells);
    t->used = false;
    int k = pos_in_order(idx);
    if (k >= 0) {
        memmove(order + k, order + k + 1, (size_t)(nterms - k - 1) * sizeof(int));
        nterms--;
    }
    if (focus == idx)
        focus = -1;
    if (fullscreen == idx)
        fullscreen = -1;
    focus_any();
    layout();
}

static void close_term(int idx)
{
    // the hangup from closing the master takes care of the foreground job
    kill(terms[idx].pid);
    remove_term(idx);
}

static void cycle_focus(int dir)
{
    if (focus < 0)
        return;
    int k = pos_in_order(focus);
    for (int step = 1; step <= nterms; step++) {
        int j = order[((k + dir * step) % nterms + nterms) % nterms];
        if (visible(j)) {
            focus = j;
            break;
        }
    }
    full_redraw = bar_dirty = true;
}

static void move_in_stack(int dir)
{
    if (focus < 0)
        return;
    int k = pos_in_order(focus);
    for (int step = 1; step < nterms; step++) {
        int j = ((k + dir * step) % nterms + nterms) % nterms;
        if (visible(order[j])) {
            int tmp = order[j];
            order[j] = order[k];
            order[k] = tmp;
            break;
        }
    }
    layout();
}

static void zoom(void)
{
    if (focus < 0)
        return;
    int k = pos_in_order(focus);
    int first = -1;
    for (int j = 0; j < nterms; j++) {
        if (visible(order[j])) {
            first = j;
            break;
        }
    }
    if (first < 0 || first == k)
        return;
    int tmp = order[first];
    order[first] = order[k];
    order[k] = tmp;
    layout();
}

static void switch_ws(int ws)
{
    if (ws == cur_ws)
        return;
    cur_ws = ws;
    focus = -1;
    fullscreen = -1;
    focus_any();
    layout();
}

static void send_to_ws(int ws)
{
    if (focus < 0 || ws == cur_ws)
        return;
    terms[focus].ws = ws;
    if (fullscreen == focus)
        fullscreen = -1;
    focus = -1;
    focus_any();
    layout();
}

// drawing

// the x display

#define X_REQ  "/tmp/.zwm-xdisplay"
#define X_SOCK "/tmp/.X11-unix/X1"
#define X_FB   "/dev/shm/Xvfb_screen0"

static long xvfb_pid = -1;
static int xconn = -1, xfb = -1;
static uint8_t xtest_op;
static uint32_t xfb_w, xfb_h, xfb_bpl, xfb_off;
static uint8_t *xfb_buf;
static uint64_t xframe_at, xtry_at;
static uint8_t xbuttons;

static uint32_t be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        long r = read(fd, (uint8_t *)buf + got, n - got);
        if (r <= 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

static int x_window(void)
{
    for (int i = 0; i < MAX_TERMS; i++)
        if (terms[i].used && terms[i].xwin)
            return i;
    return -1;
}

// xvfb with the screen the size of the work area, and a window showing it
static void x_start(void)
{
    if (xvfb_pid > 0)
        return;
    static const char *const where[] = { "/compat/linux/usr/bin/Xvfb", "/compat/apps/usr/bin/Xvfb" };
    const char *path = NULL;
    struct zk_stat st;
    for (int i = 0; i < 2 && !path; i++)
        if (stat(where[i], &st) == 0)
            path = where[i];
    int idx = free_slot();
    if (!path || idx < 0)
        return;
    // a server killed earlier leaves these behind
    unlink("/tmp/.X1-lock");
    unlink(X_SOCK);
    char geo[32];
    snprintf(geo, sizeof geo, "%dx%dx24", screen.w, screen.h - BAR_H);
    char *argv[] = { "Xvfb", ":1", "-screen", "0", geo, "-fbdir", "/dev/shm", "-extension", "GLX",
                     "-nolisten", "tcp", "-noreset", NULL };
    int log = open("/tmp/Xvfb.log", O_WRONLY | O_CREAT | O_TRUNC);
    struct zk_spawn opts = { { log, log, log }, 0 };
    xvfb_pid = spawn(path, argv, &opts);
    if (log >= 0)
        close(log);
    if (xvfb_pid < 0)
        return;

    struct term *t = &terms[idx];
    memset(t, 0, sizeof(*t));
    t->used = true;
    t->xwin = true;
    t->master = -1;
    t->pid = xvfb_pid;
    t->ws = cur_ws;
    strlcpy(t->title, "x display", sizeof t->title);
    add_window(idx);
}

// our own connection, for xtest, and the screen's file
static void x_connect(void)
{
    if (xvfb_pid < 0 || xconn >= 0 || uptime_ms() < xtry_at)
        return;
    xtry_at = uptime_ms() + 250;
    int fd = connect_unix(X_SOCK);
    if (fd < 0)
        return;
    uint8_t setup[12] = { 'l', 0, 11, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t hdr[8];
    if (write(fd, setup, sizeof setup) != sizeof setup || !read_full(fd, hdr, 8) || hdr[0] != 1) {
        close(fd);
        return;
    }
    size_t extra = (size_t)(hdr[6] | hdr[7] << 8) * 4;
    uint8_t junk[512];
    while (extra) {
        size_t n = extra < sizeof junk ? extra : sizeof junk;
        if (!read_full(fd, junk, n)) {
            close(fd);
            return;
        }
        extra -= n;
    }
    uint8_t q[16] = { 98, 0, 4, 0, 5, 0, 0, 0, 'X', 'T', 'E', 'S', 'T', 0, 0, 0 };    // QueryExtension
    uint8_t r[32];
    if (write(fd, q, sizeof q) != sizeof q || !read_full(fd, r, 32) || r[0] != 1 || !r[8]) {
        close(fd);
        return;
    }
    xtest_op = r[9];
    xconn = fd;

    xfb = open(X_FB, O_RDONLY);
    uint8_t h[100];
    if (xfb >= 0 && read(xfb, h, sizeof h) == sizeof h) {
        xfb_w = be32(h + 16);
        xfb_h = be32(h + 20);
        xfb_bpl = be32(h + 48);
        xfb_off = be32(h) + be32(h + 76) * 12;    // header, then the colormap
        free(xfb_buf);
        xfb_buf = malloc((size_t)xfb_bpl * xfb_h);
    }
}

static void x_stop(void)
{
    if (xconn >= 0)
        close(xconn);
    if (xfb >= 0)
        close(xfb);
    xconn = xfb = -1;
    xvfb_pid = -1;
}

// an xtest FakeInput: key or button press/release, or motion to x,y
static void x_input(uint8_t type, uint8_t detail, int x, int y)
{
    if (xconn < 0)
        return;
    uint8_t r[36] = { 0 };
    r[0] = xtest_op;
    r[1] = 2;
    r[2] = 9;
    r[4] = type;
    r[5] = detail;
    r[24] = (uint8_t)x;
    r[25] = (uint8_t)(x >> 8);
    r[26] = (uint8_t)y;
    r[27] = (uint8_t)(y >> 8);
    write(xconn, r, sizeof r);
}

// set 1 scancodes (0x80 for e0 ones) to linux keycodes, which x uses plus 8
static int linux_key(uint16_t code)
{
    if (!(code & 0x80))
        return code <= 0x58 ? code : 0;
    switch (code & 0x7F) {
    case 0x1C: return 96;
    case 0x1D: return 97;
    case 0x35: return 98;
    case 0x37: return 99;
    case 0x38: return 100;
    case 0x47: return 102;
    case 0x48: return 103;
    case 0x49: return 104;
    case 0x4B: return 105;
    case 0x4D: return 106;
    case 0x4F: return 107;
    case 0x50: return 108;
    case 0x51: return 109;
    case 0x52: return 110;
    case 0x53: return 111;
    case 0x5B: return 125;
    case 0x5C: return 126;
    case 0x5D: return 127;
    }
    return 0;
}

static void x_key(const struct zk_key_event *e)
{
    int k = linux_key(e->code);
    if (k)
        x_input(e->pressed ? 2 : 3, (uint8_t)(k + 8), 0, 0);
}

static void draw_xwin(struct term *t, int x0, int y0, int w, int h)
{
    if (xfb < 0 || !xfb_buf) {
        gfx_text(&screen, &font, x0 + 12, y0 + 12, "starting x...", col(cfg.dim), GFX_CLEAR, 1);
        return;
    }
    int rows = h < (int)xfb_h ? h : (int)xfb_h;
    int cols = w < (int)xfb_w ? w : (int)xfb_w;
    seek(xfb, xfb_off, 0);
    if (read(xfb, xfb_buf, (size_t)rows * xfb_bpl) <= 0)
        return;
    bool native = fbi.red_shift == 16 && fbi.green_shift == 8 && fbi.blue_shift == 0;
    for (int y = 0; y < rows; y++) {
        uint32_t *src = (uint32_t *)(xfb_buf + (size_t)y * xfb_bpl);
        uint32_t *dst = screen.px + (size_t)(y0 + y) * screen.stride + x0;
        if (native) {
            memcpy(dst, src, (size_t)cols * 4);
        } else {
            for (int x = 0; x < cols; x++)
                dst[x] = col(src[x] & 0xFFFFFF);
        }
    }
    (void)t;
}

static void draw_term(struct term *t, bool focused)
{
    uint32_t edge = col(focused ? cfg.accent : cfg.inactive);
    gfx_frame(&screen, t->x, t->y, t->w, t->h, cfg.border, edge);
    int ix = t->x + cfg.border, iy = t->y + cfg.border, iw = t->w - 2 * cfg.border;
    gfx_fill(&screen, ix, iy, iw, TITLE_H, col(focused ? cfg.title_bg : cfg.desktop));
    const char *title = t->osc_title[0] ? t->osc_title : t->title;
    gfx_text(&screen, &font, ix + 8, iy + (TITLE_H - fh) / 2, title,
             col(focused ? cfg.bar_text : cfg.dim), GFX_CLEAR, 1);

    int cx0 = ix, cy0 = iy + TITLE_H, cw = iw, ch = t->h - 2 * cfg.border - TITLE_H;
    gfx_fill(&screen, cx0, cy0, cw, ch, col(cfg.term_bg));
    if (t->xwin) {
        draw_xwin(t, cx0, cy0, cw, ch);
        t->dirty = false;
        return;
    }
    int ox = cx0 + PAD, oy = cy0 + PAD;
    for (int y = 0; y < t->rows; y++) {
        for (int x = 0; x < t->cols; x++) {
            struct cell *c = at(t, x, y);
            uint8_t fgi = c->fg, bgi = c->bg;
            if ((c->attr & ATTR_BOLD) && fgi < 8)
                fgi += 8;
            uint32_t fg = fgi == FG_DEFAULT ? cfg.term_fg : cfg.palette[fgi & 15];
            uint32_t bg = bgi == BG_DEFAULT ? cfg.term_bg : cfg.palette[bgi & 15];
            if (c->attr & ATTR_REVERSE) {
                uint32_t tmp = fg;
                fg = bg;
                bg = tmp;
            }
            bool cur = focused && t->cursor_on && x == t->cx && y == t->cy;
            if (cur) {
                fg = cfg.term_bg;
                bg = cfg.cursor;
            }
            if (c->ch == ' ' && bg == cfg.term_bg)
                continue;    // already cleared
            gfx_glyph(&screen, &font, ox + x * fw, oy + y * fh, c->ch, col(fg), col(bg), 1);
        }
    }
    t->dirty = false;
}

static void draw_wallpaper(void)
{
    if (cfg.bg_kind == BG_IMAGE && wallpaper) {
        for (int y = BAR_H; y < screen.h; y++)
            memcpy(screen.px + (size_t)y * screen.stride, wallpaper + (size_t)y * screen.w,
                   (size_t)screen.w * 4);
    } else if (cfg.bg_kind == BG_GRADIENT) {
        for (int y = BAR_H; y < screen.h; y += 4) {
            int t = (y - BAR_H) * 255 / (screen.h - BAR_H);
            gfx_fill(&screen, 0, y, screen.w, 4, col(gfx_mix(cfg.desktop, cfg.desktop2, t)));
        }
    } else {
        gfx_fill(&screen, 0, BAR_H, screen.w, screen.h - BAR_H, col(cfg.desktop));
    }
    bool empty = true;
    for (int k = 0; k < nterms; k++)
        if (visible(order[k]))
            empty = false;
    if (empty && cfg.logo[0]) {
        int scale = 6;
        int w = (int)strlen(cfg.logo) * fw * scale;
        int x = (screen.w - w) / 2, y = (screen.h - fh * scale) / 2;
        gfx_text(&screen, &font, x + 3, y + 3, cfg.logo, col(gfx_mix(cfg.desktop, 0, 160)), GFX_CLEAR, scale);
        gfx_text(&screen, &font, x, y, cfg.logo, col(gfx_mix(cfg.desktop, cfg.accent, 70)), GFX_CLEAR, scale);
    }
}

static uint64_t mem_total, mem_free;

static void read_meminfo(void)
{
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd < 0)
        return;
    char buf[256];
    long n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';
    char *p = strstr(buf, "MemTotal:");
    if (p)
        mem_total = (uint64_t)strtol(p + 9, NULL, 10) * 1024;
    p = strstr(buf, "MemFree:");
    if (p)
        mem_free = (uint64_t)strtol(p + 8, NULL, 10) * 1024;
}

static void draw_bar(void)
{
    gfx_fill(&screen, 0, 0, screen.w, BAR_H, col(cfg.bar));
    int y = (BAR_H - fh) / 2;
    int x = 6;
    for (int ws = 0; ws < NWS; ws++) {
        bool used = false;
        for (int k = 0; k < nterms; k++)
            if (terms[order[k]].ws == ws)
                used = true;
        if (!used && ws != cur_ws)
            continue;
        char label[2] = { (char)('1' + ws), 0 };
        int w = fw + 12;
        if (ws == cur_ws)
            gfx_fill(&screen, x, 0, w, BAR_H, col(cfg.accent));
        gfx_text(&screen, &font, x + 6, y, label, col(ws == cur_ws ? cfg.bar : cfg.bar_text),
                 GFX_CLEAR, 1);
        x += w;
    }
    x += 10;
    gfx_text(&screen, &font, x, y, "ZOS", col(cfg.accent), GFX_CLEAR, 1);
    x += 4 * fw;
    if (focus >= 0) {
        struct term *t = &terms[focus];
        gfx_text(&screen, &font, x + 10, y, t->osc_title[0] ? t->osc_title : t->title,
                 col(cfg.bar_text), GFX_CLEAR, 1);
    }

    struct datetime d;
    epoch_to_datetime(time(), &d);
    static const char *days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char used[16], total[16], right[96];
    format_size(mem_total - mem_free, used, sizeof used);
    format_size(mem_total, total, sizeof total);
    char clock[16];
    if (cfg.clock_24h)
        snprintf(clock, sizeof clock, "%02d:%02d", d.hour, d.min);
    else
        snprintf(clock, sizeof clock, "%d:%02d %s", d.hour % 12 ? d.hour % 12 : 12, d.min,
                 d.hour < 12 ? "am" : "pm");
    char mem[48] = "";
    if (cfg.show_memory)
        snprintf(mem, sizeof mem, "mem %s/%s   ", used, total);
    snprintf(right, sizeof right, "%s%s %s %d  %s", mem, days[d.wday], mon[d.month - 1], d.day, clock);
    int rw = (int)strlen(right) * fw;
    gfx_text(&screen, &font, screen.w - rw - 10, y, right, col(cfg.bar_text), GFX_CLEAR, 1);
}

static void blit(int x, int y, int w, int h)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > screen.w) w = screen.w - x;
    if (y + h > screen.h) h = screen.h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int row = y; row < y + h; row++)
        memcpy((uint8_t *)fbmem + (size_t)row * fbi.pitch + (size_t)x * 4,
               screen.px + (size_t)row * screen.stride + x, (size_t)w * 4);
}

// 1 = outline, 2 = fill
static const char *const arrow[] = {
    "1",
    "11",
    "121",
    "1221",
    "12221",
    "122221",
    "1222221",
    "12222221",
    "122222221",
    "1222222221",
    "12222222221",
    "122222211111",
    "1222221",
    "122112221",
    "12 1 1221",
    "11   1221",
    "      1221",
    "       11",
};
#define ARROW_W 12
#define ARROW_H 18

static void draw_pointer(void)
{
    // put back what was under the old one, then draw on the framebuffer directly
    blit(old_mx, old_my, ARROW_W, ARROW_H);
    uint32_t outline = col(0x000000), fill = col(0xffffff);
    for (int y = 0; y < ARROW_H; y++) {
        for (int x = 0; arrow[y][x]; x++) {
            int px = mouse_x + x, py = mouse_y + y;
            if (arrow[y][x] == ' ' || px >= screen.w || py >= screen.h)
                continue;
            uint32_t *p = (uint32_t *)((uint8_t *)fbmem + (size_t)py * fbi.pitch) + px;
            *p = arrow[y][x] == '1' ? outline : fill;
        }
    }
    old_mx = mouse_x;
    old_my = mouse_y;
}

static void render(void)
{
    bool any = false;
    if (full_redraw) {
        draw_wallpaper();
        draw_bar();
        for (int k = nterms - 1; k >= 0; k--)
            if (visible(order[k]) && (fullscreen < 0 || order[k] == fullscreen))
                draw_term(&terms[order[k]], order[k] == focus);
        blit(0, 0, screen.w, screen.h);
        full_redraw = bar_dirty = false;
        any = true;
    } else {
        if (bar_dirty) {
            draw_bar();
            blit(0, 0, screen.w, BAR_H);
            bar_dirty = false;
            any = true;
        }
        for (int i = 0; i < MAX_TERMS; i++) {
            struct term *t = &terms[i];
            if (!visible(i) || !t->dirty || (fullscreen >= 0 && i != fullscreen))
                continue;
            draw_term(t, i == focus);
            blit(t->x, t->y, t->w, t->h);
            any = true;
        }
    }
    if (any || pointer_moved)
        draw_pointer();
    pointer_moved = false;
}

// input

static void send(struct term *t, const char *s, size_t n)
{
    write(t->master, s, n);
}

static void key_to_term(const struct zk_key_event *e)
{
    if (focus < 0)
        return;
    struct term *t = &terms[focus];
    if (t->xwin) {
        x_key(e);
        return;
    }
    switch (e->key) {
    case ZK_KEY_UP:     send(t, "\e[A", 3); return;
    case ZK_KEY_DOWN:   send(t, "\e[B", 3); return;
    case ZK_KEY_RIGHT:  send(t, "\e[C", 3); return;
    case ZK_KEY_LEFT:   send(t, "\e[D", 3); return;
    case ZK_KEY_HOME:   send(t, "\e[H", 3); return;
    case ZK_KEY_END:    send(t, "\e[F", 3); return;
    case ZK_KEY_PGUP:   send(t, "\e[5~", 4); return;
    case ZK_KEY_PGDN:   send(t, "\e[6~", 4); return;
    case ZK_KEY_DELETE: send(t, "\e[3~", 4); return;
    }
    if (!e->ch)
        return;
    char c = (char)e->ch;
    if (c == '\n')
        c = '\r';
    else if (c == '\b')
        c = 0x7f;
    send(t, &c, 1);
}

static bool running = true;

static void handle_key(const struct zk_key_event *e)
{
    if (!e->pressed) {
        if (focus >= 0 && terms[focus].xwin)
            x_key(e);
        return;
    }
    bool mod = e->mods & cfg.mods;
    bool shift = e->mods & KEYMOD_SHIFT;
    uint16_t c = e->code;
    // user bindings come first, they can override the built in ones
    uint32_t held = e->mods & (KEYMOD_SUPER | KEYMOD_ALT | KEYMOD_SHIFT | KEYMOD_CTRL);
    for (int i = 0; i < cfg.nbinds; i++) {
        if (cfg.binds[i].code == c && cfg.binds[i].mods == held) {
            spawn_term_cmd(cfg.binds[i].cmd);
            return;
        }
    }
    if (!mod) {
        key_to_term(e);
        return;
    }
    if (c >= 0x02 && c <= 0x0A) {    // 1..9
        if (shift)
            send_to_ws(c - 0x02);
        else
            switch_ws(c - 0x02);
        return;
    }
    switch (c) {
    case 0x1C:    // enter
        if (shift)
            zoom();
        else
            spawn_term();
        return;
    case 0x10:    // q
        if (focus >= 0)
            close_term(focus);
        return;
    case 0x24:    // j
    case 0xD0:    // down
    case 0xCD:    // right
    case 0x0F:    // tab
        if (shift && c == 0x24)
            move_in_stack(1);
        else
            cycle_focus(1);
        return;
    case 0x25:    // k
    case 0xC8:    // up
    case 0xCB:    // left
        if (shift && c == 0x25)
            move_in_stack(-1);
        else
            cycle_focus(-1);
        return;
    case 0x23:    // h
        if (mfact[cur_ws] > 20)
            mfact[cur_ws] -= 5;
        layout();
        return;
    case 0x26:    // l
        if (mfact[cur_ws] < 80)
            mfact[cur_ws] += 5;
        layout();
        return;
    case 0x21:    // f
        fullscreen = fullscreen == focus ? -1 : focus;
        layout();
        return;
    case 0x12:    // e
        if (shift)
            running = false;
        return;
    case 0x13:    // r
        if (shift) {
            load_config();
            load_wallpaper();
            load_font();
            layout();    // a new font size changes the grid, resize keeps the text
            full_redraw = true;
            return;
        }
        break;
    }
    key_to_term(e);    // not ours, let the terminal have it
}

static int term_at(int x, int y)
{
    for (int k = 0; k < nterms; k++) {
        int i = order[k];
        if (!visible(i) || (fullscreen >= 0 && i != fullscreen))
            continue;
        struct term *t = &terms[i];
        if (x >= t->x && x < t->x + t->w && y >= t->y && y < t->y + t->h)
            return i;
    }
    return -1;
}

static void handle_mouse(const struct zk_mouse_event *e)
{
    mouse_x += e->dx;
    mouse_y += e->dy;
    if (mouse_x < 0) mouse_x = 0;
    if (mouse_y < 0) mouse_y = 0;
    if (mouse_x >= screen.w) mouse_x = screen.w - 1;
    if (mouse_y >= screen.h) mouse_y = screen.h - 1;
    if (e->dx || e->dy)
        pointer_moved = true;

    // over the x display: motion and buttons go to x
    int over = term_at(mouse_x, mouse_y);
    if (over >= 0 && terms[over].xwin) {
        struct term *t = &terms[over];
        int cx = mouse_x - t->x - cfg.border, cy = mouse_y - t->y - cfg.border - TITLE_H;
        if (cy >= 0) {
            if (e->dx || e->dy)
                x_input(6, 0, cx, cy);
            static const uint8_t bits[3] = { MOUSE_LEFT, MOUSE_MIDDLE, MOUSE_RIGHT };
            for (int b = 0; b < 3; b++)
                if ((e->buttons ^ xbuttons) & bits[b])
                    x_input(e->buttons & bits[b] ? 4 : 5, (uint8_t)(b + 1), 0, 0);
            xbuttons = e->buttons;
        }
    }

    bool left_down = (e->buttons & MOUSE_LEFT) && !(mouse_buttons & MOUSE_LEFT);
    mouse_buttons = e->buttons;
    if (!left_down)
        return;
    if (mouse_y < BAR_H) {
        // workspace tags, same positions draw_bar uses
        int x = 6;
        for (int ws = 0; ws < NWS; ws++) {
            bool used = ws == cur_ws;
            for (int k = 0; k < nterms; k++)
                if (terms[order[k]].ws == ws)
                    used = true;
            if (!used)
                continue;
            if (mouse_x >= x && mouse_x < x + fw + 12) {
                switch_ws(ws);
                return;
            }
            x += fw + 12;
        }
        return;
    }
    int i = term_at(mouse_x, mouse_y);
    if (i >= 0 && i != focus) {
        int old = focus;
        focus = i;
        if (old >= 0)
            terms[old].dirty = true;
        terms[i].dirty = true;
        bar_dirty = true;
    }
}

// titles follow whatever runs in the foreground of each terminal
static void update_titles(void)
{
    static struct zk_procinfo info[64];
    int n = ps(info, 64);
    if (n < 0)
        return;
    for (int i = 0; i < MAX_TERMS; i++) {
        struct term *t = &terms[i];
        if (!t->used || t->xwin)
            continue;
        uint64_t fg = 0;
        ioctl(t->master, TIOCGFG, (uint64_t)&fg);
        const char *name = NULL;
        for (int k = 0; k < n && !name; k++)
            if ((fg && info[k].pgid == fg && info[k].pid == fg) || (!fg && (long)info[k].pid == t->pid))
                name = info[k].name;
        for (int k = 0; k < n && !name && fg; k++)
            if (info[k].pgid == fg)
                name = info[k].name;
        if (name && strcmp(name, t->title) != 0) {
            strlcpy(t->title, name, sizeof t->title);
            t->dirty = true;
            if (i == focus)
                bar_dirty = true;
        }
    }
}

static void reap(void)
{
    int status;
    long pid;
    while ((pid = wait(-1, &status, WNOHANG)) > 0) {
        if (pid == xvfb_pid)
            x_stop();
        for (int i = 0; i < MAX_TERMS; i++)
            if (terms[i].used && terms[i].pid == pid)
                remove_term(i);
    }
}

int main(void)
{
    // xrun finds us here when it wants the screen for x
    int pfd = open("/tmp/.zwm.pid", O_WRONLY | O_CREAT | O_TRUNC);
    if (pfd >= 0) {
        dprintf(pfd, "%ld\n", getpid());
        close(pfd);
    }
    int fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) {
        dprintf(2, "zwm: /dev/fb0: %s\n", strerror(fbfd));
        return 1;
    }
    uint64_t addr = 0;
    if (ioctl(fbfd, FBIOGETINFO, (uint64_t)&fbi) < 0 || ioctl(fbfd, FBIOMAP, (uint64_t)&addr) < 0) {
        dprintf(2, "zwm: can't map the framebuffer\n");
        return 1;
    }
    fbmem = (uint32_t *)addr;
    gfx_set_format(&fbi);

    load_config();
    load_font();
    if (!fw) {
        dprintf(2, "zwm: no font\n");
        return 1;
    }

    screen.w = (int)fbi.width;
    screen.h = (int)fbi.height;
    screen.stride = screen.w;
    screen.px = malloc((size_t)screen.w * screen.h * 4);
    if (!screen.px) {
        dprintf(2, "zwm: out of memory\n");
        return 1;
    }
    load_wallpaper();

    int kbd = open("/dev/kbd", O_RDONLY);
    if (kbd < 0) {
        dprintf(2, "zwm: /dev/kbd: %s\n", strerror(kbd));
        return 1;
    }
    int mouse = open("/dev/mouse", O_RDONLY);
    mouse_x = screen.w / 2;
    mouse_y = screen.h / 2;
    setenv("TERM", "vt100", 1);
    read_meminfo();
    if (cfg.nautostart)
        for (int i = 0; i < cfg.nautostart; i++)
            spawn_term_cmd(cfg.autostart[i]);
    else
        spawn_term();

    uint64_t last_tick = 0;
    static char buf[READ_CHUNK];
    while (running) {
        struct zk_pollfd fds[3 + MAX_TERMS];
        int map[3 + MAX_TERMS];
        int n = 0;
        fds[n++] = (struct zk_pollfd){ kbd, POLLIN, 0 };
        if (mouse >= 0)
            fds[n++] = (struct zk_pollfd){ mouse, POLLIN, 0 };
        int first_term = n;
        for (int i = 0; i < MAX_TERMS; i++) {
            if (terms[i].used) {
                map[n] = i;
                fds[n++] = (struct zk_pollfd){ terms[i].master, POLLIN, 0 };
            }
        }

        int end_term = n;
        // the x connection only ever brings errors and events we don't use
        int xslot = -1;
        if (xconn >= 0) {
            xslot = n;
            fds[n++] = (struct zk_pollfd){ xconn, POLLIN, 0 };
        }
        int xw = x_window();
        poll(fds, (size_t)n, xw >= 0 || xvfb_pid > 0 ? 50 : 1000);
        if (xslot >= 0 && (fds[xslot].revents & (POLLIN | POLLHUP))) {
            if (read(xconn, buf, sizeof buf) <= 0)
                x_stop();
        }

        // xrun asking for the x display
        struct zk_stat xst;
        if (stat(X_REQ, &xst) == 0) {
            unlink(X_REQ);
            x_start();
        }
        x_connect();
        if (xw >= 0 && visible(xw) && uptime_ms() >= xframe_at) {
            terms[xw].dirty = true;
            xframe_at = uptime_ms() + 66;
        }

        if (fds[0].revents & POLLIN) {
            struct zk_key_event ev[16];
            long got = read(kbd, ev, sizeof ev);
            for (long k = 0; k < got / (long)sizeof ev[0] && running; k++)
                handle_key(&ev[k]);
        }
        if (mouse >= 0 && (fds[1].revents & POLLIN)) {
            struct zk_mouse_event ev[32];
            long got = read(mouse, ev, sizeof ev);
            for (long k = 0; k < got / (long)sizeof ev[0]; k++)
                handle_mouse(&ev[k]);
        }
        for (int k = first_term; k < end_term; k++) {
            int i = map[k];
            if (!terms[i].used || terms[i].master != fds[k].fd)
                continue;    // closed by a key above
            if (fds[k].revents & POLLIN) {
                long got = read(terms[i].master, buf, sizeof buf);
                if (got > 0) {
                    for (long b = 0; b < got; b++)
                        feed(&terms[i], (unsigned char)buf[b]);
                } else {
                    close_term(i);
                    continue;
                }
            } else if (fds[k].revents & POLLHUP) {
                close_term(i);
            }
        }
        reap();

        uint64_t now = uptime_ms();
        if (now - last_tick >= 1000) {
            last_tick = now;
            read_meminfo();
            update_titles();
            bar_dirty = true;
        }
        if (running)
            render();
    }

    for (int i = 0; i < MAX_TERMS; i++)
        if (terms[i].used)
            close_term(i);
    reap();
    return 0;
}
