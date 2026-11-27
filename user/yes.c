#include "zk.h"

int main(int argc, char **argv)
{
    const char *s = argc > 1 ? argv[1] : "y";
    char buf[1024];
    size_t len = strlen(s), n = 0;
    while (n + len + 1 <= sizeof buf) {
        memcpy(buf + n, s, len);
        buf[n + len] = '\n';
        n += len + 1;
    }
    if (n == 0)
        return 1;
    while (write_all(1, buf, n) > 0)
        ;
    return 0;
}
