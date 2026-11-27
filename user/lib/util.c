#include "zk.h"

int isdigit(int c) { return c >= '0' && c <= '9'; }
int isalpha(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
int isalnum(int c) { return isdigit(c) || isalpha(c); }
int isspace(int c) { return c == ' ' || (c >= '\t' && c <= '\r'); }
int isprint(int c) { return c >= 0x20 && c < 0x7f; }
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }
int toupper(int c) { return islower(c) ? c - 32 : c; }
int tolower(int c) { return isupper(c) ? c + 32 : c; }

long strtol(const char *s, char **end, int base)
{
    while (isspace(*s))
        s++;
    int neg = 0;
    if (*s == '-' || *s == '+')
        neg = *s++ == '-';
    if ((base == 0 || base == 16) && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
    } else if (base == 0) {
        base = s[0] == '0' ? 8 : 10;
    }
    unsigned long v = 0;
    for (;; s++) {
        int d;
        if (isdigit(*s))
            d = *s - '0';
        else if (isalpha(*s))
            d = tolower(*s) - 'a' + 10;
        else
            break;
        if (d >= base)
            break;
        v = v * (unsigned long)base + (unsigned long)d;
    }
    if (end)
        *end = (char *)s;
    return neg ? -(long)v : (long)v;
}

char *strstr(const char *h, const char *n)
{
    size_t nl = strlen(n);
    if (!nl)
        return (char *)h;
    for (; *h; h++)
        if (*h == *n && strncmp(h, n, nl) == 0)
            return (char *)h;
    return NULL;
}

char *strdup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

int strcasecmp(const char *a, const char *b)
{
    for (; *a && tolower(*a) == tolower(*b); a++, b++)
        ;
    return tolower((unsigned char)*a) - tolower((unsigned char)*b);
}

// merge sort, so it's stable
static void msort(char *base, char *tmp, size_t n, size_t size,
                  int (*cmp)(const void *, const void *))
{
    if (n < 2)
        return;
    size_t half = n / 2;
    msort(base, tmp, half, size, cmp);
    msort(base + half * size, tmp, n - half, size, cmp);
    size_t i = 0, j = half, k = 0;
    while (i < half && j < n) {
        if (cmp(base + j * size, base + i * size) < 0)
            memcpy(tmp + k++ * size, base + j++ * size, size);
        else
            memcpy(tmp + k++ * size, base + i++ * size, size);
    }
    while (i < half)
        memcpy(tmp + k++ * size, base + i++ * size, size);
    while (j < n)
        memcpy(tmp + k++ * size, base + j++ * size, size);
    memcpy(base, tmp, n * size);
}

void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *))
{
    char *tmp = malloc(n * size);
    if (!tmp)
        return;
    msort(base, tmp, n, size, cmp);
    free(tmp);
}

struct lreader *lr_open(int fd)
{
    struct lreader *r = calloc(1, sizeof(*r));
    if (r)
        r->fd = fd;
    return r;
}

long lr_getline(struct lreader *r, char **line)
{
    size_t n = 0;
    for (;;) {
        if (r->pos == r->len) {
            long got = read(r->fd, r->buf, sizeof r->buf);
            if (got <= 0) {
                if (n == 0)
                    return got < 0 ? got : -1;
                break;
            }
            r->pos = 0;
            r->len = (size_t)got;
        }
        char c = r->buf[r->pos++];
        if (n + 2 > r->cap) {
            size_t cap = r->cap ? r->cap * 2 : 128;
            char *l = realloc(r->line, cap);
            if (!l)
                return ZK_ENOMEM;
            r->line = l;
            r->cap = cap;
        }
        if (c == '\n')
            break;
        r->line[n++] = c;
    }
    r->line[n] = '\0';
    *line = r->line;
    return (long)n;
}

void lr_close(struct lreader *r)
{
    if (r) {
        free(r->line);
        free(r);
    }
}

int open_arg(const char *prog, const char *path)
{
    if (strcmp(path, "-") == 0)
        return 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        dprintf(2, "%s: %s: %s\n", prog, path, strerror(fd));
    return fd;
}

const char *basename_of(const char *path)
{
    const char *s = strrchr(path, '/');
    return s && s[1] ? s + 1 : path;
}

void format_size(uint64_t bytes, char *out, size_t size)
{
    const char *units = "BKMGT";
    int u = 0;
    uint64_t whole = bytes, frac = 0;
    while (whole >= 1024 && u < 4) {
        frac = (whole % 1024) * 10 / 1024;
        whole /= 1024;
        u++;
    }
    if (u == 0)
        snprintf(out, size, "%luB", whole);
    else if (whole < 10)
        snprintf(out, size, "%lu.%lu%c", whole, frac, units[u]);
    else
        snprintf(out, size, "%lu%c", whole, units[u]);
}
