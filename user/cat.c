#include "zk.h"

static int copy(int fd, const char *name)
{
    char buf[1024];
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0)
        if (write_all(1, buf, (size_t)n) < 0)
            return 1;
    if (n < 0) {
        dprintf(2, "cat: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return copy(0, "stdin");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-") == 0) {
            rc |= copy(0, "stdin");
            continue;
        }
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "cat: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        rc |= copy(fd, argv[i]);
        close(fd);
    }
    return rc;
}
