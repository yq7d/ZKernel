// uniq [-c] [file]
#include "zk.h"

int main(int argc, char **argv)
{
    int i = 1;
    bool counts = false;
    if (i < argc && strcmp(argv[i], "-c") == 0) {
        counts = true;
        i++;
    }
    int fd = i < argc ? open_arg("uniq", argv[i]) : 0;
    if (fd < 0)
        return 1;
    struct lreader *r = lr_open(fd);
    char *line, *prev = NULL;
    long n = 0;
    for (;;) {
        bool eof = lr_getline(r, &line) < 0;
        if (prev && (eof || strcmp(line, prev) != 0)) {
            if (counts)
                printf("%7ld %s\n", n, prev);
            else
                printf("%s\n", prev);
            free(prev);
            prev = NULL;
        }
        if (eof)
            break;
        if (!prev) {
            prev = strdup(line);
            n = 0;
        }
        n++;
    }
    lr_close(r);
    return 0;
}
