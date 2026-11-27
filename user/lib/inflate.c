// deflate and gzip decoding (rfc 1951, 1952)
#include "zk.h"

struct bits {
    const uint8_t *in;
    size_t len, pos;
    uint32_t buf;
    int count;
    uint8_t *out;
    size_t olen, ocap;
    bool err;
};

struct huff {
    uint16_t count[16];
    uint16_t symbol[320];
};

static int bit(struct bits *b)
{
    if (b->count == 0) {
        if (b->pos >= b->len) {
            b->err = true;
            return 0;
        }
        b->buf = b->in[b->pos++];
        b->count = 8;
    }
    int v = (int)(b->buf & 1);
    b->buf >>= 1;
    b->count--;
    return v;
}

static int getbits(struct bits *b, int n)
{
    int v = 0;
    for (int i = 0; i < n; i++)
        v |= bit(b) << i;
    return v;
}

static bool put(struct bits *b, uint8_t c)
{
    if (b->olen == b->ocap) {
        size_t cap = b->ocap ? b->ocap * 2 : 65536;
        uint8_t *o = realloc(b->out, cap);
        if (!o) {
            b->err = true;
            return false;
        }
        b->out = o;
        b->ocap = cap;
    }
    b->out[b->olen++] = c;
    return true;
}

static void build(struct huff *h, const uint8_t *lengths, int n)
{
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[lengths[i]]++;
    h->count[0] = 0;
    offs[1] = 0;
    for (int i = 1; i < 15; i++)
        offs[i + 1] = (uint16_t)(offs[i] + h->count[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i])
            h->symbol[offs[lengths[i]]++] = (uint16_t)i;
}

static int decode(struct bits *b, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= bit(b);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->err)
            return -1;
    }
    b->err = true;
    return -1;
}

static const uint16_t len_base[] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                     35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
                                     3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
                                      257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
                                      8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
                                      7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static bool codes(struct bits *b, const struct huff *lit, const struct huff *dist)
{
    for (;;) {
        int sym = decode(b, lit);
        if (b->err || sym < 0)
            return false;
        if (sym < 256) {
            if (!put(b, (uint8_t)sym))
                return false;
        } else if (sym == 256) {
            return true;
        } else {
            sym -= 257;
            if (sym >= 29)
                return false;
            int len = len_base[sym] + getbits(b, len_extra[sym]);
            int ds = decode(b, dist);
            if (ds < 0 || ds >= 30)
                return false;
            size_t d = dist_base[ds] + (size_t)getbits(b, dist_extra[ds]);
            if (d > b->olen)
                return false;
            for (int i = 0; i < len; i++)
                if (!put(b, b->out[b->olen - d]))
                    return false;
        }
    }
}

static bool fixed(struct bits *b)
{
    static struct huff lit, dist;
    static bool ready;
    if (!ready) {
        uint8_t l[288];
        for (int i = 0; i < 144; i++) l[i] = 8;
        for (int i = 144; i < 256; i++) l[i] = 9;
        for (int i = 256; i < 280; i++) l[i] = 7;
        for (int i = 280; i < 288; i++) l[i] = 8;
        build(&lit, l, 288);
        for (int i = 0; i < 30; i++) l[i] = 5;
        build(&dist, l, 30);
        ready = true;
    }
    return codes(b, &lit, &dist);
}

static bool dynamic(struct bits *b)
{
    static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    int nlen = getbits(b, 5) + 257, ndist = getbits(b, 5) + 1, ncode = getbits(b, 4) + 4;
    if (nlen > 286 || ndist > 30)
        return false;
    uint8_t lengths[320];
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < ncode; i++)
        lengths[order[i]] = (uint8_t)getbits(b, 3);
    struct huff lencode, lit, dist;
    build(&lencode, lengths, 19);
    int i = 0;
    while (i < nlen + ndist) {
        int sym = decode(b, &lencode);
        if (sym < 0 || b->err)
            return false;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int rep, val = 0;
        if (sym == 16) {
            if (i == 0)
                return false;
            val = lengths[i - 1];
            rep = 3 + getbits(b, 2);
        } else if (sym == 17) {
            rep = 3 + getbits(b, 3);
        } else {
            rep = 11 + getbits(b, 7);
        }
        if (i + rep > nlen + ndist)
            return false;
        while (rep--)
            lengths[i++] = (uint8_t)val;
    }
    build(&lit, lengths, nlen);
    build(&dist, lengths + nlen, ndist);
    return codes(b, &lit, &dist);
}

// one raw deflate stream. returns how much input it used, or an error
static long inflate_raw(struct bits *b)
{
    int last;
    do {
        last = bit(b);
        int type = getbits(b, 2);
        if (b->err)
            return ZK_EIO;
        if (type == 0) {
            b->count = 0;    // to a byte boundary
            if (b->pos + 4 > b->len)
                return ZK_EIO;
            uint16_t len = (uint16_t)(b->in[b->pos] | b->in[b->pos + 1] << 8);
            b->pos += 4;
            if (b->pos + len > b->len)
                return ZK_EIO;
            for (uint16_t i = 0; i < len; i++)
                if (!put(b, b->in[b->pos + i]))
                    return ZK_ENOMEM;
            b->pos += len;
        } else if (type == 1) {
            if (!fixed(b))
                return ZK_EIO;
        } else if (type == 2) {
            if (!dynamic(b))
                return ZK_EIO;
        } else {
            return ZK_EIO;
        }
    } while (!last);
    return (long)b->pos;
}

long gunzip_member(const uint8_t *in, size_t len, uint8_t **out, size_t *olen)
{
    if (len < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8)
        return ZK_EINVAL;
    uint8_t flags = in[3];
    size_t pos = 10;
    if (flags & 4) {
        if (pos + 2 > len)
            return ZK_EIO;
        pos += 2 + (size_t)(in[pos] | in[pos + 1] << 8);
    }
    if (flags & 8)
        while (pos < len && in[pos++])
            ;
    if (flags & 16)
        while (pos < len && in[pos++])
            ;
    if (flags & 2)
        pos += 2;
    if (pos >= len)
        return ZK_EIO;

    struct bits b = { in + pos, len - pos, 0, 0, 0, *out, *olen, *olen, false };
    // keep appending to what's already there
    b.ocap = *olen;
    long used = inflate_raw(&b);
    *out = b.out;
    *olen = b.olen;
    if (used < 0)
        return used;
    size_t end = pos + (size_t)used + 8;    // crc32 and size trailer
    return end > len ? ZK_EIO : (long)end;
}

// sha1 and base64, apk checksums are "Q1" + base64(sha1)

#define ROL(x, n) (((x) << (n)) | ((x) >> (32 - (n))))

void sha1(const void *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    const uint8_t *p = data;
    uint64_t bits = (uint64_t)len * 8;
    size_t total = ((len + 8) / 64 + 1) * 64;
    for (size_t off = 0; off < total; off += 64) {
        uint8_t blk[64];
        for (int i = 0; i < 64; i++) {
            size_t at = off + (size_t)i;
            if (at < len)
                blk[i] = p[at];
            else if (at == len)
                blk[i] = 0x80;
            else if (at >= total - 8)
                blk[i] = (uint8_t)(bits >> (8 * (total - 1 - at)));
            else
                blk[i] = 0;
        }
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)blk[i * 4] << 24 | (uint32_t)blk[i * 4 + 1] << 16 |
                   (uint32_t)blk[i * 4 + 2] << 8 | blk[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }
            uint32_t t = ROL(a, 5) + f + e + k + w[i];
            e = d; d = c; c = ROL(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    for (int i = 0; i < 5; i++) {
        out[i * 4] = (uint8_t)(h[i] >> 24);
        out[i * 4 + 1] = (uint8_t)(h[i] >> 16);
        out[i * 4 + 2] = (uint8_t)(h[i] >> 8);
        out[i * 4 + 3] = (uint8_t)h[i];
    }
}

void base64(const uint8_t *in, size_t len, char *out)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) v |= (uint32_t)in[i + 1] << 8;
        if (i + 2 < len) v |= in[i + 2];
        out[o++] = t[v >> 18 & 63];
        out[o++] = t[v >> 12 & 63];
        out[o++] = i + 1 < len ? t[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? t[v & 63] : '=';
    }
    out[o] = '\0';
}
