// cp [-r] src... dst
#include "zk.h"

static bool recursive;

static int copy_file(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY);
    if (in < 0) {
        dprintf(2, "cp: %s: %s\n", src, strerror(in));
        return 1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        dprintf(2, "cp: %s: %s\n", dst, strerror(out));
        close(in);
        return 1;
    }
    static char buf[16384];
    long n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0) {
        long w = write_all(out, buf, (size_t)n);
        if (w < 0) {
            dprintf(2, "cp: %s: %s\n", dst, strerror((int)w));
            rc = 1;
            break;
        }
    }
    if (n < 0) {
        dprintf(2, "cp: %s: %s\n", src, strerror((int)n));
        rc = 1;
    }
    close(in);
    close(out);
    return rc;
}

static int copy(const char *src, const char *dst)
{
    struct zk_stat st;
    int err = stat(src, &st);
    if (err) {
        dprintf(2, "cp: %s: %s\n", src, strerror(err));
        return 1;
    }
    if (st.type != ZK_TYPE_DIR)
        return copy_file(src, dst);
    if (!recursive) {
        dprintf(2, "cp: %s is a directory (use -r)\n", src);
        return 1;
    }
    err = mkdir(dst);
    if (err && err != ZK_EEXIST) {
        dprintf(2, "cp: %s: %s\n", dst, strerror(err));
        return 1;
    }
    int fd = open(src, O_RDONLY);
    if (fd < 0)
        return 1;
    int rc = 0;
    struct zk_dirent d;
    while (readdir(fd, &d) > 0) {
        char s[ZK_PATH_MAX], t[ZK_PATH_MAX];
        snprintf(s, sizeof s, "%s/%s", src, d.name);
        snprintf(t, sizeof t, "%s/%s", dst, d.name);
        rc |= copy(s, t);
    }
    close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && strcmp(argv[i], "-r") == 0) {
        recursive = true;
        i++;
    }
    if (argc - i < 2) {
        dprintf(2, "usage: cp [-r] src... dst\n");
        return 2;
    }
    const char *dst = argv[argc - 1];
    struct zk_stat st;
    bool into_dir = stat(dst, &st) == 0 && st.type == ZK_TYPE_DIR;
    if (argc - i > 2 && !into_dir) {
        dprintf(2, "cp: %s is not a directory\n", dst);
        return 1;
    }
    int rc = 0;
    for (; i < argc - 1; i++) {
        char target[ZK_PATH_MAX];
        if (into_dir)
            snprintf(target, sizeof target, "%s/%s", dst, basename_of(argv[i]));
        else
            strlcpy(target, dst, sizeof target);
        rc |= copy(argv[i], target);
    }
    return rc;
}
