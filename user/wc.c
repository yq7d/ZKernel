#include "zk.h"

static int count(int fd, const char *name)
{
    char buf[1024];
    uint64_t lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    long n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        bytes += (uint64_t)n;
        for (long i = 0; i < n; i++) {
            char c = buf[i];
            if (c == '\n')
                lines++;
            bool space = c == ' ' || c == '\n' || c == '\t';
            if (!space && !in_word)
                words++;
            in_word = !space;
        }
    }
    if (n < 0) {
        dprintf(2, "wc: %s: %s\n", name, strerror((int)n));
        return 1;
    }
    printf("%7lu %7lu %7lu %s\n", lines, words, bytes, name);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return count(0, "");
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_RDONLY);
        if (fd < 0) {
            dprintf(2, "wc: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        rc |= count(fd, argv[i]);
        close(fd);
    }
    return rc;
}
