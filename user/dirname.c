#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: dirname path\n");
        return 2;
    }
    char buf[ZK_PATH_MAX];
    strlcpy(buf, argv[1], sizeof buf);
    size_t n = strlen(buf);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = '\0';
    char *s = strrchr(buf, '/');
    if (!s)
        printf(".\n");
    else if (s == buf)
        printf("/\n");
    else {
        *s = '\0';
        printf("%s\n", buf);
    }
    return 0;
}
