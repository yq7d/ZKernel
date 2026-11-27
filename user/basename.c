#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: basename path [suffix]\n");
        return 2;
    }
    char buf[ZK_PATH_MAX];
    strlcpy(buf, argv[1], sizeof buf);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0';
    const char *b = basename_of(buf);
    char out[ZK_PATH_MAX];
    strlcpy(out, b, sizeof out);
    if (argc > 2) {
        size_t ol = strlen(out), sl = strlen(argv[2]);
        if (sl < ol && strcmp(out + ol - sl, argv[2]) == 0)
            out[ol - sl] = '\0';
    }
    printf("%s\n", out);
    return 0;
}
