// tail [-n N] [file]
#include "zk.h"

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
    if (n <= 0)
        return 0;
    int fd = i < argc ? open_arg("tail", argv[i]) : 0;
    if (fd < 0)
        return 1;

    // ring of the last n lines
    char **ring = calloc((size_t)n, sizeof(char *));
    if (!ring)
        return 1;
    struct lreader *r = lr_open(fd);
    char *line;
    long count = 0;
    while (lr_getline(r, &line) >= 0) {
        free(ring[count % n]);
        ring[count % n] = strdup(line);
        count++;
    }
    long start = count > n ? count - n : 0;
    for (long k = start; k < count; k++)
        printf("%s\n", ring[k % n]);
    lr_close(r);
    return 0;
}
