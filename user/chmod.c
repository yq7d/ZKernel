// chmod mode file..., mode is octal or like u+x,go-w,a=r
#include "zk.h"

static bool apply(const char *spec, uint32_t old, uint32_t *out)
{
    if (isdigit(*spec)) {
        char *end;
        long v = strtol(spec, &end, 8);
        if (*end || v < 0 || v > 07777)
            return false;
        *out = (uint32_t)v;
        return true;
    }
    uint32_t mode = old;
    const char *p = spec;
    while (*p) {
        uint32_t who = 0;
        for (; *p && strchr("ugoa", *p); p++)
            who |= *p == 'u' ? 04700 : *p == 'g' ? 02070 : *p == 'o' ? 01007 : 07777;
        if (!who)
            who = 07777;
        char op = *p++;
        if (op != '+' && op != '-' && op != '=')
            return false;
        uint32_t bits = 0;
        for (; *p && *p != ','; p++) {
            if (*p == 'r') bits |= 0444;
            else if (*p == 'w') bits |= 0222;
            else if (*p == 'x') bits |= 0111;
            else if (*p == 's') bits |= 06000;
            else if (*p == 't') bits |= 01000;
            else return false;
        }
        bits &= who;
        if (op == '+')
            mode |= bits;
        else if (op == '-')
            mode &= ~bits;
        else
            mode = (mode & ~(who & 0777)) | bits;
        if (*p == ',')
            p++;
    }
    *out = mode & 07777;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(2, "usage: chmod mode file...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 2; i < argc; i++) {
        struct zk_stat st;
        uint32_t mode;
        int err = stat(argv[i], &st);
        if (!err && !apply(argv[1], st.mode, &mode)) {
            dprintf(2, "chmod: bad mode %s\n", argv[1]);
            return 2;
        }
        if (!err)
            err = chmod(argv[i], mode);
        if (err) {
            dprintf(2, "chmod: %s: %s\n", argv[i], strerror(err));
            rc = 1;
        }
    }
    return rc;
}
