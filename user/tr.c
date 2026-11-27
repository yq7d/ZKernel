// tr [-d] set1 [set2], with a-z style ranges
#include "zk.h"

static char unescape(const char **s)
{
    if (**s != '\\' || !(*s)[1])
        return *(*s);
    (*s)++;
    switch (**s) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return '\0';
    default:  return **s;
    }
}

static size_t expand(const char *src, char *out)
{
    // escapes first, then ranges
    char tmp[256];
    size_t t = 0;
    for (const char *p = src; *p && t < 255; p++)
        tmp[t++] = unescape(&p);
    tmp[t] = '\0';
    const char *s = tmp;
    size_t n = 0;
    for (; (size_t)(s - tmp) < t && n < 255; s++) {
        if (s[1] == '-' && s[2]) {
            for (int c = (unsigned char)s[0]; c <= (unsigned char)s[2] && n < 255; c++)
                out[n++] = (char)c;
            s += 2;
        } else {
            out[n++] = *s;
        }
    }
    return n;
}

int main(int argc, char **argv)
{
    bool del = argc > 1 && strcmp(argv[1], "-d") == 0;
    int i = del ? 2 : 1;
    if (argc - i < (del ? 1 : 2)) {
        dprintf(2, "usage: tr [-d] set1 [set2]\n");
        return 2;
    }
    char from[256], to[256];
    size_t nf = expand(argv[i], from), nt = del ? 0 : expand(argv[i + 1], to);
    int map[256];
    for (int c = 0; c < 256; c++)
        map[c] = c;
    for (size_t k = 0; k < nf; k++)
        map[(unsigned char)from[k]] = del ? -1 : (unsigned char)to[k < nt ? k : nt - 1];

    char buf[1024], out[1024];
    long n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        size_t o = 0;
        for (long k = 0; k < n; k++) {
            int c = map[(unsigned char)buf[k]];
            if (c >= 0)
                out[o++] = (char)c;
        }
        write_all(1, out, o);
    }
    return 0;
}
