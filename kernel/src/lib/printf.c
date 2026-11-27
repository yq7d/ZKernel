#include "lib/printf.h"
#include "lib/string.h"
#include <stdbool.h>
#include <stdint.h>

struct spec {
    bool left, zero, plus, space, alt;
    int width;
    int prec;       // -1 if unset
};

struct sink {
    putc_fn out;
    void *ctx;
    int count;
};

static void emit(struct sink *s, char c)
{
    s->out(c, s->ctx);
    s->count++;
}

static void emit_pad(struct sink *s, char c, int n)
{
    while (n-- > 0)
        emit(s, c);
}

static void emit_str(struct sink *s, const struct spec *sp, const char *str)
{
    if (!str)
        str = "(null)";
    size_t len = strnlen(str, sp->prec >= 0 ? (size_t)sp->prec : SIZE_MAX);
    int pad = sp->width - (int)len;

    if (!sp->left)
        emit_pad(s, ' ', pad);
    for (size_t i = 0; i < len; i++)
        emit(s, str[i]);
    if (sp->left)
        emit_pad(s, ' ', pad);
}

static void emit_num(struct sink *s, const struct spec *sp, uint64_t v,
                     bool negative, unsigned base, bool upper)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[64];
    int nd = 0;

    if (!(v == 0 && sp->prec == 0)) {
        do {
            buf[nd++] = digits[v % base];
            v /= base;
        } while (v);
    }

    char prefix[3];
    int np = 0;
    if (negative)
        prefix[np++] = '-';
    else if (sp->plus)
        prefix[np++] = '+';
    else if (sp->space)
        prefix[np++] = ' ';
    if (sp->alt && nd > 0) {
        if (base == 16) {
            prefix[np++] = '0';
            prefix[np++] = upper ? 'X' : 'x';
        } else if (base == 2) {
            prefix[np++] = '0';
            prefix[np++] = 'b';
        } else if (base == 8 && buf[nd - 1] != '0') {
            prefix[np++] = '0';
        }
    }

    int zeros = 0;
    if (sp->prec >= 0)
        zeros = sp->prec - nd;
    else if (sp->zero && !sp->left)
        zeros = sp->width - np - nd;
    if (zeros < 0)
        zeros = 0;

    int pad = sp->width - np - zeros - nd;

    if (!sp->left)
        emit_pad(s, ' ', pad);
    for (int i = 0; i < np; i++)
        emit(s, prefix[i]);
    emit_pad(s, '0', zeros);
    while (nd)
        emit(s, buf[--nd]);
    if (sp->left)
        emit_pad(s, ' ', pad);
}

enum length { LEN_HH, LEN_H, LEN_INT, LEN_L, LEN_LL, LEN_Z };

int kvformat(putc_fn out, void *ctx, const char *fmt, va_list ap)
{
    struct sink s = { out, ctx, 0 };
    va_list args;
    va_copy(args, ap);

    while (*fmt) {
        if (*fmt != '%') {
            emit(&s, *fmt++);
            continue;
        }
        const char *start = fmt++;
        struct spec sp = { .prec = -1 };

        for (;; fmt++) {
            if      (*fmt == '-') sp.left  = true;
            else if (*fmt == '0') sp.zero  = true;
            else if (*fmt == '+') sp.plus  = true;
            else if (*fmt == ' ') sp.space = true;
            else if (*fmt == '#') sp.alt   = true;
            else break;
        }

        if (*fmt == '*') {
            sp.width = va_arg(args, int);
            if (sp.width < 0) {
                sp.left = true;
                sp.width = -sp.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                sp.width = sp.width * 10 + (*fmt++ - '0');
        }

        if (*fmt == '.') {
            fmt++;
            sp.prec = 0;
            if (*fmt == '*') {
                sp.prec = va_arg(args, int);
                if (sp.prec < 0)
                    sp.prec = -1;
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    sp.prec = sp.prec * 10 + (*fmt++ - '0');
            }
        }

        enum length len = LEN_INT;
        if (*fmt == 'h') {
            fmt++;
            len = LEN_H;
            if (*fmt == 'h') { fmt++; len = LEN_HH; }
        } else if (*fmt == 'l') {
            fmt++;
            len = LEN_L;
            if (*fmt == 'l') { fmt++; len = LEN_LL; }
        } else if (*fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            fmt++;
            len = LEN_Z;
        }

        char conv = *fmt;
        if (conv == '\0') {
            while (start < fmt)
                emit(&s, *start++);
            break;
        }
        fmt++;

        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v;
            switch (len) {
            case LEN_HH:  v = (signed char)va_arg(args, int); break;
            case LEN_H:   v = (short)va_arg(args, int); break;
            case LEN_INT: v = va_arg(args, int); break;
            default:      v = va_arg(args, long); break;
            }
            uint64_t mag = v < 0 ? -(uint64_t)v : (uint64_t)v;
            emit_num(&s, &sp, mag, v < 0, 10, false);
            break;
        }
        case 'u': case 'x': case 'X': case 'o': case 'b': {
            uint64_t v;
            switch (len) {
            case LEN_HH:  v = (unsigned char)va_arg(args, unsigned); break;
            case LEN_H:   v = (unsigned short)va_arg(args, unsigned); break;
            case LEN_INT: v = va_arg(args, unsigned); break;
            default:      v = va_arg(args, unsigned long); break;
            }
            unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8
                          : conv == 'b' ? 2 : 16;
            emit_num(&s, &sp, v, false, base, conv == 'X');
            break;
        }
        case 'p': {
            struct spec p = sp;
            p.alt = true;
            p.prec = 16;
            emit_num(&s, &p, (uintptr_t)va_arg(args, void *), false, 16, false);
            break;
        }
        case 'c': {
            char c = (char)va_arg(args, int);
            if (!sp.left)
                emit_pad(&s, ' ', sp.width - 1);
            emit(&s, c);
            if (sp.left)
                emit_pad(&s, ' ', sp.width - 1);
            break;
        }
        case 's':
            emit_str(&s, &sp, va_arg(args, const char *));
            break;
        case '%':
            emit(&s, '%');
            break;
        default:
            while (start < fmt)
                emit(&s, *start++);
            break;
        }
    }

    va_end(args);
    return s.count;
}

struct bufctx {
    char *buf;
    size_t size;
    size_t pos;
};

static void buf_putc(char c, void *ctx)
{
    struct bufctx *b = ctx;
    if (b->pos + 1 < b->size)
        b->buf[b->pos] = c;
    b->pos++;
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct bufctx b = { buf, size, 0 };
    int n = kvformat(buf_putc, &b, fmt, ap);
    if (size)
        buf[b.pos < size ? b.pos : size - 1] = '\0';
    return n;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
