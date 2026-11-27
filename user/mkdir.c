// mkdir [-p] dir...
#include "zk.h"

static int make(char *path, bool parents)
{
    if (parents) {
        for (char *p = path + 1; *p; p++) {
            if (*p != '/')
                continue;
            *p = '\0';
            struct zk_stat st;
            int err = stat(path, &st) == 0 && st.type == ZK_TYPE_DIR ? 0 : mkdir(path);
            if (err && err != ZK_EEXIST) {
                dprintf(2, "mkdir: %s: %s\n", path, strerror(err));
                *p = '/';
                return 1;
            }
            *p = '/';
        }
    }
    int err = mkdir(path);
    if (err && !(parents && err == ZK_EEXIST)) {
        dprintf(2, "mkdir: %s: %s\n", path, strerror(err));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    bool parents = false;
    if (i < argc && strcmp(argv[i], "-p") == 0) {
        parents = true;
        i++;
    }
    if (i == argc) {
        dprintf(2, "usage: mkdir [-p] dir...\n");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++)
        rc |= make(argv[i], parents);
    return rc;
}
