// head [-n N] [file...]
#include "zk.h"

static int head(int fd, long lines)
{
    struct lreader *r = lr_open(fd);
    char *line;
    for (long i = 0; i < lines && lr_getline(r, &line) >= 0; i++)
        printf("%s\n", line);
    lr_close(r);
    return 0;
}

int main(int argc, char **argv)
{
    long n = 10;
    int i = 1;
    if (i + 1 < argc && strcmp(argv[i], "-n") == 0) {
        n = strtol(argv[i + 1], NULL, 10);
        i += 2;
    } else if (i < argc && argv[i][0] == '-' && isdigit(argv[i][1])) {
        n = strtol(argv[i] + 1, NULL, 10);
        i++;
    }
    if (i == argc)
        return head(0, n);
    int rc = 0;
    for (; i < argc; i++) {
        int fd = open_arg("head", argv[i]);
        if (fd < 0) {
            rc = 1;
            continue;
        }
        head(fd, n);
        if (fd)
            close(fd);
    }
    return rc;
}
