// rm [-r] [-f] path...
#include "zk.h"

static bool recursive, force;

static int remove(const char *path)
{
    struct zk_stat st;
    int err = stat(path, &st);
    if (!err && st.type == ZK_TYPE_DIR && recursive) {
        int fd = open(path, O_RDONLY);
        if (fd >= 0) {
            // removing entries shifts the rest, so keep reading the first one
            struct zk_dirent d;
            char names[16][ZK_NAME_MAX + 1];
            for (;;) {
                seek(fd, 0, SEEK_SET);
                int n = 0;
                while (n < 16 && readdir(fd, &d) > 0)
                    strlcpy(names[n++], d.name, sizeof names[0]);
                if (n == 0)
                    break;
                int failed = 0;
                for (int i = 0; i < n; i++) {
                    char child[ZK_PATH_MAX];
                    snprintf(child, sizeof child, "%s/%s", path, names[i]);
                    failed |= remove(child);
                }
                if (failed)
                    break;
            }
            close(fd);
        }
    }
    err = unlink(path);
    if (err && !(force && err == ZK_ENOENT)) {
        dprintf(2, "rm: %s: %s\n", path, strerror(err));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        for (const char *p = argv[i] + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') recursive = true;
            else if (*p == 'f') force = true;
            else {
                dprintf(2, "usage: rm [-rf] path...\n");
                return 2;
            }
        }
    }
    if (i == argc && !force) {
        dprintf(2, "usage: rm [-rf] path...\n");
        return 2;
    }
    int rc = 0;
    for (; i < argc; i++)
        rc |= remove(argv[i]);
    return rc;
}
