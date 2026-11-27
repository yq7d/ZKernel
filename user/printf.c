// printf format [args...]: %s %d %i %u %x %c %%, and \n \t \e \NNN \xHH escapes
#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: printf format [args...]\n");
        return 2;
    }
    const char *f = argv[1];
    int arg = 2;
    char out[4096];
    size_t n = 0;
#define PUT(c) do { if (n + 1 < sizeof out) out[n++] = (c); } while (0)
    for (; *f; f++) {
        if (*f == '\\' && f[1]) {
            f++;
            if (*f >= '0' && *f <= '7') {
                // \NNN or \0NNN, octal
                int v = 0, digits = *f == '0' ? 4 : 3;
                for (int k = 0; k < digits && *f >= '0' && *f <= '7'; k++, f++)
                    v = v * 8 + (*f - '0');
                f--;
                PUT((char)v);
            } else if (*f == 'x' && isalnum(f[1])) {
                int v = 0;
                for (int k = 0; k < 2 && isalnum(f[1]); k++) {
                    char h = (char)tolower(*++f);
                    v = v * 16 + (h <= '9' ? h - '0' : h - 'a' + 10);
                }
                PUT((char)v);
            } else {
                PUT(*f == 'n' ? '\n' : *f == 't' ? '\t' : *f == 'r' ? '\r' :
                    *f == 'e' ? '\e' : *f == 'a' ? '\a' : *f);
            }
            continue;
        }
        if (*f != '%' || !f[1]) {
            PUT(*f);
            continue;
        }
        f++;
        // pass flags and width through to snprintf
        char spec[16] = "%";
        size_t sl = 1;
        while (*f && strchr("-+ 0#123456789.", *f) && sl < 12)
            spec[sl++] = *f++;
        const char *a = arg < argc ? argv[arg] : "";
        char tmp[512];
        switch (*f) {
        case '%':
            PUT('%');
            continue;
        case 's':
            spec[sl++] = 's';
            spec[sl] = '\0';
            snprintf(tmp, sizeof tmp, spec, a);
            break;
        case 'c':
            tmp[0] = a[0];
            tmp[1] = '\0';
            break;
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            spec[sl++] = 'l';
            spec[sl++] = *f == 'i' ? 'd' : *f;
            spec[sl] = '\0';
            snprintf(tmp, sizeof tmp, spec, strtol(a, NULL, 0));
            break;
        default:
            PUT('%');
            PUT(*f);
            continue;
        }
        arg++;
        for (char *t = tmp; *t; t++)
            PUT(*t);
    }
    write_all(1, out, n);
    return 0;
}
