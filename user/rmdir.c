#include "zk.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: rmdir dir...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct zk_stat st;
        int err = stat(argv[i], &st);
        if (!err && st.type != ZK_TYPE_DIR)
            err = ZK_ENOTDIR;
        if (!err)
            err = unlink(argv[i]);
        if (err) {
            dprintf(2, "rmdir: %s: %s\n", argv[i], strerror(err));
            rc = 1;
        }
    }
    return rc;
}
