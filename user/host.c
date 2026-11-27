#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: host name\n");
        return 2;
    }
    uint32_t ip;
    int err = resolve(argv[1], &ip);
    if (err) {
        dprintf(2, "host: %s: %s\n", argv[1], strerror(err));
        return 1;
    }
    char s[16];
    inet_format(ip, s, sizeof s);
    printf("%s has address %s\n", argv[1], s);
    return 0;
}
