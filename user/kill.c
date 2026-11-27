#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: kill <pid...>\n");
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int err = kill(atoi(argv[i]));
        if (err) {
            dprintf(2, "kill: %s: %s\n", argv[i], strerror(err));
            rc = 1;
        }
    }
    return rc;
}
