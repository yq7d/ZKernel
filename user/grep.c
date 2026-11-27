// grep [-i] [-v] [-n] [-c] pattern [file...], fixed strings
#include "zk.h"

static bool icase, invert, numbers, count_only;

static bool matches(const char *line, const char *pat)
{
    if (!icase)
        return strstr(line, pat) != NULL;
    size_t pl = strlen(pat);
    for (; *line; line++) {
        size_t k = 0;
        while (k < pl && line[k] && tolower(line[k]) == tolower(pat[k]))
            k++;
        if (k == pl)
            return true;
    }
    return pl == 0;
}

static long grep(int fd, const char *pat, const char *name)
{
    struct lreader *r = lr_open(fd);
    char *line;
    long lineno = 0, hits = 0;
    while (lr_getline(r, &line) >= 0) {
        lineno++;
        if (matches(line, pat) == invert)
            continue;
        hits++;
        if (count_only)
            continue;
        if (name)
            printf("\e[35m%s\e[0m:", name);
        if (numbers)
            printf("\e[32m%ld\e[0m:", lineno);
        printf("%s\n", line);
    }
    lr_close(r);
    if (count_only) {
        if (name)
            printf("%s:", name);
        printf("%ld\n", hits);
    }
    return hits;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'i') icase = true;
            else if (*p == 'v') invert = true;
            else if (*p == 'n') numbers = true;
            else if (*p == 'c') count_only = true;
            else {
                dprintf(2, "usage: grep [-ivnc] pattern [file...]\n");
                return 2;
            }
        }
    }
    if (i == argc) {
        dprintf(2, "usage: grep [-ivnc] pattern [file...]\n");
        return 2;
    }
    const char *pat = argv[i++];
    long hits = 0;
    int rc = 0;
    if (i == argc)
        hits = grep(0, pat, NULL);
    bool many = argc - i > 1;
    for (; i < argc; i++) {
        int fd = open_arg("grep", argv[i]);
        if (fd < 0) {
            rc = 2;
            continue;
        }
        hits += grep(fd, pat, many ? argv[i] : NULL);
        if (fd)
            close(fd);
    }
    return rc ? rc : hits ? 0 : 1;
}
