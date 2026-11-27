#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: touch file...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_WRONLY | O_CREAT | O_APPEND);
        if (fd < 0) {
            dprintf(2, "touch: %s: %s\n", argv[i], strerror(fd));
            rc = 1;
            continue;
        }
        // an empty append still bumps the mtime on most filesystems here
        struct zk_stat st;
        if (fstat(fd, &st) == 0)
            ftruncate(fd, st.size);
        close(fd);
    }
    return rc;
}
