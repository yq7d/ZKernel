// mv src... dst. across filesystems it copies and removes
#include "zk.h"

static int run(char **argv)
{
    long pid = spawn(argv[0], argv, NULL);
    if (pid < 0)
        return 1;
    int status = 1;
    wait(pid, &status, 0);
    return status;
}

static int move(const char *src, const char *dst)
{
    int err = rename(src, dst);
    if (err == ZK_EXDEV) {
        char *cp[] = { "/bin/cp", "-r", (char *)src, (char *)dst, NULL };
        char *rm[] = { "/bin/rm", "-r", (char *)src, NULL };
        return run(cp) || run(rm);
    }
    if (err) {
        dprintf(2, "mv: %s -> %s: %s\n", src, dst, strerror(err));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        dprintf(2, "usage: mv src... dst\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct zk_stat st;
    bool into_dir = stat(dst, &st) == 0 && st.type == ZK_TYPE_DIR;
    if (argc > 3 && !into_dir) {
        dprintf(2, "mv: %s is not a directory\n", dst);
        return 1;
    }
    int rc = 0;
    for (int i = 1; i < argc - 1; i++) {
        char target[ZK_PATH_MAX];
        if (into_dir)
            snprintf(target, sizeof target, "%s/%s", dst, basename_of(argv[i]));
        else
            strlcpy(target, dst, sizeof target);
        rc |= move(argv[i], target);
    }
    return rc;
}
