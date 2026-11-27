#include "zk.h"

int main(int argc, char **argv)
{
    if (argc != 2) {
        dprintf(2, "usage: umount target\n");
        return 2;
    }
    int err = umount(argv[1]);
    if (err) {
        dprintf(2, "umount: %s: %s\n", argv[1], strerror(err));
        return 1;
    }
    return 0;
}
