#include "lib/font.h"

#define PSF1_MAGIC0     0x36
#define PSF1_MAGIC1     0x04
#define PSF1_MODE512    0x01
#define PSF1_MODEHASTAB 0x02
#define PSF1_SEPARATOR  0xFFFF
#define PSF1_STARTSEQ   0xFFFE

#define PSF2_MAGIC      0x864ab572
#define PSF2_HAS_UNICODE_TABLE 0x01
#define PSF2_SEPARATOR  0xFF
#define PSF2_STARTSEQ   0xFE

struct psf1_header {
    uint8_t magic[2];
    uint8_t mode;
    uint8_t charsize;
} __attribute__((packed));

struct psf2_header {
    uint32_t magic;
    uint32_t version;
    uint32_t headersize;
    uint32_t flags;
    uint32_t length;
    uint32_t charsize;
    uint32_t height, width;
} __attribute__((packed));

static void map_identity(struct font *f)
{
    for (unsigned i = 0; i < 256; i++)
        f->map[i] = i < f->glyph_count ? i : '?';
}

// ucs-2 entries per glyph, 0xFFFF terminated. sequences are skipped
static void parse_psf1_table(struct font *f, const uint8_t *p, const uint8_t *end)
{
    for (uint32_t glyph = 0; glyph < f->glyph_count && p + 2 <= end; glyph++) {
        int in_seq = 0;
        for (; p + 2 <= end; p += 2) {
            uint16_t cp = (uint16_t)(p[0] | (p[1] << 8));
            if (cp == PSF1_SEPARATOR) { p += 2; break; }
            if (cp == PSF1_STARTSEQ) in_seq = 1;
            else if (!in_seq && cp < 256) f->map[cp] = (uint16_t)glyph;
        }
    }
}

// same but utf-8
static void parse_psf2_table(struct font *f, const uint8_t *p, const uint8_t *end)
{
    for (uint32_t glyph = 0; glyph < f->glyph_count && p < end; glyph++) {
        int in_seq = 0;
        while (p < end) {
            uint8_t b = *p;
            if (b == PSF2_SEPARATOR) { p++; break; }
            if (b == PSF2_STARTSEQ) { in_seq = 1; p++; continue; }

            uint32_t cp;
            if (b < 0x80)                 { cp = b; p += 1; }
            else if ((b & 0xE0) == 0xC0 && p + 1 < end)
                                          { cp = ((b & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
            else if ((b & 0xF0) == 0xE0)  { cp = 0x10000; p += 3; }
            else                          { cp = 0x10000; p += 4; }

            if (!in_seq && cp < 256)
                f->map[cp] = (uint16_t)glyph;
        }
    }
}

bool font_load_psf(struct font *f, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    const uint8_t *end = bytes + size;

    if (size >= sizeof(struct psf1_header) &&
        bytes[0] == PSF1_MAGIC0 && bytes[1] == PSF1_MAGIC1) {
        const struct psf1_header *h = data;
        f->width = 8;
        f->height = h->charsize;
        f->glyph_count = (h->mode & PSF1_MODE512) ? 512 : 256;
        f->bytes_per_row = 1;
        f->bytes_per_glyph = h->charsize;
        f->glyphs = bytes + sizeof(*h);
        if (f->glyphs + f->glyph_count * f->bytes_per_glyph > end)
            return false;
        map_identity(f);
        if (h->mode & PSF1_MODEHASTAB)
            parse_psf1_table(f, f->glyphs + f->glyph_count * f->bytes_per_glyph, end);
        return true;
    }

    if (size >= sizeof(struct psf2_header)) {
        const struct psf2_header *h = data;
        if (h->magic != PSF2_MAGIC)
            return false;
        f->width = h->width;
        f->height = h->height;
        f->glyph_count = h->length;
        f->bytes_per_row = (h->width + 7) / 8;
        f->bytes_per_glyph = h->charsize;
        f->glyphs = bytes + h->headersize;
        if (f->glyphs + (size_t)f->glyph_count * f->bytes_per_glyph > end)
            return false;
        map_identity(f);
        if (h->flags & PSF2_HAS_UNICODE_TABLE)
            parse_psf2_table(f, f->glyphs + (size_t)f->glyph_count * f->bytes_per_glyph, end);
        return true;
    }

    return false;
}

const uint8_t *font_glyph(const struct font *f, unsigned char c)
{
    return f->glyphs + (size_t)f->map[c] * f->bytes_per_glyph;
}
