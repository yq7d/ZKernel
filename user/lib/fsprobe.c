// what filesystem a device holds, and its label
#include "zk.h"

int probe_fs(const char *dev, char *type, size_t tsize, char *label, size_t lsize)
{
    int fd = open(dev, O_RDONLY);
    if (fd < 0)
        return fd;
    static uint8_t buf[2048];
    long n = read(fd, buf, sizeof buf);
    close(fd);
    if (n < (long)sizeof buf)
        return ZK_EIO;
    if (buf[1024 + 56] == 0x53 && buf[1024 + 57] == 0xEF) {
        strlcpy(type, "ext2", tsize);
        size_t k = 0;
        while (k < 16 && k + 1 < lsize && buf[1024 + 120 + k])
            label[k] = (char)buf[1024 + 120 + k], k++;
        label[k] = '\0';
        return 0;
    }
    if (buf[510] == 0x55 && buf[511] == 0xAA && memcmp(buf + 82, "FAT32", 5) == 0) {
        strlcpy(type, "fat", tsize);
        size_t k = 0;
        while (k < 11 && k + 1 < lsize)
            label[k] = (char)buf[71 + k], k++;
        while (k && label[k - 1] == ' ')
            k--;
        label[k] = '\0';
        return 0;
    }
    return ZK_EINVAL;
}

int find_label(const char *label, char *dev, size_t size, char *type, size_t tsize)
{
    int fd = open("/proc/disks", O_RDONLY);
    if (fd < 0)
        return fd;
    struct lreader *r = lr_open(fd);
    char *line;
    int err = ZK_ENOENT;
    while (err && lr_getline(r, &line) >= 0) {
        char *sp = strchr(line, ' ');
        if (!sp || line[0] == '#')
            continue;
        *sp = '\0';
        char path[64], l[32], t[16];
        snprintf(path, sizeof path, "/dev/%s", line);
        if (probe_fs(path, t, sizeof t, l, sizeof l) == 0 && strcmp(l, label) == 0) {
            strlcpy(dev, path, size);
            strlcpy(type, t, tsize);
            err = 0;
        }
    }
    lr_close(r);
    close(fd);
    return err;
}
