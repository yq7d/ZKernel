#include "zk.h"
#include "lib/time.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        dprintf(2, "usage: stat path...\n");
        return 2;
    }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        struct zk_stat st;
        int err = stat(argv[i], &st);
        if (err) {
            dprintf(2, "stat: %s: %s\n", argv[i], strerror(err));
            rc = 1;
            continue;
        }
        static const char *types[] = { "?", "regular file", "directory", "device", "pipe" };
        struct datetime d;
        epoch_to_datetime(st.mtime, &d);
        printf("  File: %s\n  Type: %s\n  Size: %lu\n Inode: %lu\n  Mode: %04o  Uid: %u  Gid: %u\nModify: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
               argv[i], types[st.type < 5 ? st.type : 0], st.size, st.ino, st.mode, st.uid, st.gid,
               d.year, d.month, d.day, d.hour, d.min, d.sec);
    }
    return rc;
}
