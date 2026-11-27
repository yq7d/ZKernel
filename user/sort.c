// sort [-r] [-n] [-u] [file...]
#include "zk.h"

static bool reverse, numeric, unique;

static int cmp(const void *a, const void *b)
{
    const char *x = *(char *const *)a, *y = *(char *const *)b;
    int r;
    if (numeric) {
        long p = strtol(x, NULL, 10), q = strtol(y, NULL, 10);
        r = p < q ? -1 : p > q;
    } else {
        r = strcmp(x, y);
    }
    return reverse ? -r : r;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r') reverse = true;
            else if (*p == 'n') numeric = true;
            else if (*p == 'u') unique = true;
            else {
                dprintf(2, "usage: sort [-rnu] [file...]\n");
                return 2;
            }
        }
    }
    size_t n = 0, cap = 256;
    char **lines = malloc(cap * sizeof(char *));
    int first = i;
    do {
        int fd = i < argc ? open_arg("sort", argv[i]) : 0;
        if (fd < 0)
            return 1;
        struct lreader *r = lr_open(fd);
        char *line;
        while (lr_getline(r, &line) >= 0) {
            if (n == cap) {
                cap *= 2;
                lines = realloc(lines, cap * sizeof(char *));
                if (!lines)
                    return 1;
            }
            lines[n++] = strdup(line);
        }
        lr_close(r);
        if (fd)
            close(fd);
        i++;
    } while (i < argc);
    (void)first;

    qsort(lines, n, sizeof(char *), cmp);
    for (size_t k = 0; k < n; k++) {
        if (unique && k && cmp(&lines[k], &lines[k - 1]) == 0)
            continue;
        printf("%s\n", lines[k]);
    }
    return 0;
}
