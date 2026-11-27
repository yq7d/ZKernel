// mount                              list mounts
// mount -a                           everything in /etc/fstab
// mount [-o opts] [-t type] src dst  one filesystem
#include "cat_proc.h"

static void mkdirs(const char *path)
{
    char buf[ZK_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(buf);
        *p = '/';
    }
    mkdir(buf);
}

static int mount_all(void)
{
    int fd = open("/etc/fstab", O_RDONLY);
    if (fd < 0) {
        dprintf(2, "mount: /etc/fstab: %s\n", strerror(fd));
        return 1;
    }
    struct lreader *r = lr_open(fd);
    char *line;
    int rc = 0;
    while (lr_getline(r, &line) >= 0) {
        char *f[4] = { 0 }, *save;
        int n = 0;
        for (char *w = strtok_r(line, " \t", &save); w && n < 4; w = strtok_r(NULL, " \t", &save))
            f[n++] = w;
        if (n < 3 || f[0][0] == '#')
            continue;
        const char *opts = n > 3 && strcmp(f[3], "defaults") != 0 ? f[3] : "";
        // LABEL=name finds the disk wherever it is, and is skipped quietly if
        // it isn't plugged in
        char dev[64], type[16];
        if (strncmp(f[0], "LABEL=", 6) == 0) {
            if (find_label(f[0] + 6, dev, sizeof dev, type, sizeof type) != 0)
                continue;
            f[0] = dev;
        }
        if (strcmp(f[2], "bind") == 0) {
            // only bind directories of a real filesystem, not the ram one
            struct zk_statfs st;
            char parent[ZK_PATH_MAX];
            strlcpy(parent, f[0], sizeof parent);
            char *slash = strrchr(parent, '/');
            if (slash && slash != parent)
                *slash = '\0';
            if (statfs(parent, &st) != 0 || strcmp(st.fstype, "tmpfs") == 0)
                continue;
            mkdirs(f[0]);
        }
        mkdirs(f[1]);
        int err = mount(f[0], f[1], f[2], opts);
        if (err && err != ZK_EBUSY) {
            dprintf(2, "mount: %s on %s: %s\n", f[0], f[1], strerror(err));
            rc = 1;
        }
    }
    lr_close(r);
    close(fd);
    return rc;
}

int main(int argc, char **argv)
{
    if (argc == 1)
        return cat_file("/proc/mounts");
    if (argc == 2 && strcmp(argv[1], "-a") == 0)
        return mount_all();
    const char *type = NULL;
    const char *opts = "";
    int i = 1;
    while (i + 1 < argc && argv[i][0] == '-') {
        if (strcmp(argv[i], "-o") == 0)
            opts = argv[i + 1];
        else if (strcmp(argv[i], "-t") == 0)
            type = argv[i + 1];
        else
            break;
        i += 2;
    }
    if (argc - i != 2) {
        dprintf(2, "usage: mount [-a] [-o opts] [-t type] source target\n");
        return 2;
    }
    char found[16], label[32];
    if (!type) {
        if (probe_fs(argv[i], found, sizeof found, label, sizeof label) != 0) {
            dprintf(2, "mount: %s: can't tell what filesystem that is, use -t\n", argv[i]);
            return 1;
        }
        type = found;
    }
    int err = mount(argv[i], argv[i + 1], type, opts);
    if (err) {
        dprintf(2, "mount: %s on %s: %s\n", argv[i], argv[i + 1], strerror(err));
        return 1;
    }
    return 0;
}
