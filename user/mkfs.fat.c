// mkfs.fat [-n LABEL] /dev/xxx: a fresh fat32 filesystem on a whole device
#include "zk.h"

#define SECTOR 512
#define RESERVED 32
#define NFATS 2

static int dev;

static int put(uint64_t sector, const void *buf, size_t len)
{
    if (seek(dev, (long)(sector * SECTOR), SEEK_SET) < 0)
        return ZK_EIO;
    return write_all(dev, buf, len) < 0 ? ZK_EIO : 0;
}

static uint64_t device_sectors(const char *path)
{
    // /proc/disks has "name bytes kind model"
    const char *name = basename_of(path);
    int fd = open("/proc/disks", O_RDONLY);
    if (fd < 0)
        return 0;
    struct lreader *r = lr_open(fd);
    char *line;
    uint64_t bytes = 0;
    while (lr_getline(r, &line) >= 0) {
        size_t n = strlen(name);
        if (strncmp(line, name, n) == 0 && line[n] == ' ')
            bytes = (uint64_t)strtol(line + n + 1, NULL, 10);
    }
    lr_close(r);
    close(fd);
    return bytes / SECTOR;
}

int main(int argc, char **argv)
{
    const char *label = "NO NAME", *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc)
            label = argv[++i];
        else
            path = argv[i];
    }
    if (!path) {
        dprintf(2, "usage: mkfs.fat [-n LABEL] /dev/device\n");
        return 2;
    }
    uint64_t total = device_sectors(path);
    if (total < 66600) {
        dprintf(2, "mkfs.fat: %s is too small for fat32 (needs 33 MiB)\n", path);
        return 1;
    }
    if (total > 0xFFFFFFFFUL)
        total = 0xFFFFFFFFUL;
    dev = open(path, O_RDWR);
    if (dev < 0) {
        dprintf(2, "mkfs.fat: %s: %s\n", path, strerror(dev));
        return 1;
    }

    uint32_t spc = total <= 532480 ? 1 : total <= 16777216 ? 8 : total <= 33554432 ? 16 :
                   total <= 67108864 ? 32 : 64;
    // fat size, the way the spec computes it
    uint64_t tmp1 = total - RESERVED, tmp2 = (256UL * spc + NFATS) / 2;
    uint32_t fatsz = (uint32_t)((tmp1 + tmp2 - 1) / tmp2);
    uint64_t data = RESERVED + (uint64_t)NFATS * fatsz;
    uint32_t clusters = (uint32_t)((total - data) / spc);

    uint8_t bs[SECTOR];
    memset(bs, 0, sizeof bs);
    bs[0] = 0xEB; bs[1] = 0x58; bs[2] = 0x90;
    memcpy(bs + 3, "ZOS     ", 8);
    *(uint16_t *)(bs + 11) = SECTOR;
    bs[13] = (uint8_t)spc;
    *(uint16_t *)(bs + 14) = RESERVED;
    bs[16] = NFATS;
    bs[21] = 0xF8;
    *(uint16_t *)(bs + 24) = 63;
    *(uint16_t *)(bs + 26) = 255;
    *(uint32_t *)(bs + 32) = (uint32_t)total;
    *(uint32_t *)(bs + 36) = fatsz;
    *(uint32_t *)(bs + 44) = 2;    // root dir cluster
    *(uint16_t *)(bs + 48) = 1;    // fsinfo
    *(uint16_t *)(bs + 50) = 6;    // backup boot sector
    bs[64] = 0x80;
    bs[66] = 0x29;
    uint32_t serial;
    getrandom(&serial, sizeof serial);
    *(uint32_t *)(bs + 67) = serial;
    char vol[11];
    memset(vol, ' ', 11);
    for (int i = 0; i < 11 && label[i]; i++)
        vol[i] = (char)toupper(label[i]);
    memcpy(bs + 71, vol, 11);
    memcpy(bs + 82, "FAT32   ", 8);
    bs[510] = 0x55;
    bs[511] = 0xAA;

    uint8_t fsinfo[SECTOR];
    memset(fsinfo, 0, sizeof fsinfo);
    *(uint32_t *)fsinfo = 0x41615252;
    *(uint32_t *)(fsinfo + 484) = 0x61417272;
    *(uint32_t *)(fsinfo + 488) = clusters - 1;
    *(uint32_t *)(fsinfo + 492) = 3;
    *(uint32_t *)(fsinfo + 508) = 0xAA550000;

    // clear the reserved area and both fats
    static uint8_t zero[64 * 1024];
    for (uint64_t s = 0; s < data; s += sizeof zero / SECTOR) {
        uint64_t n = data - s < sizeof zero / SECTOR ? data - s : sizeof zero / SECTOR;
        if (put(s, zero, (size_t)n * SECTOR)) {
            dprintf(2, "mkfs.fat: write failed\n");
            return 1;
        }
    }
    int err = put(0, bs, SECTOR) | put(1, fsinfo, SECTOR) | put(6, bs, SECTOR) | put(7, fsinfo, SECTOR);
    uint32_t fat0[3] = { 0x0FFFFFF8, 0x0FFFFFFF, 0x0FFFFFFF };
    for (int f = 0; f < NFATS; f++)
        err |= put(RESERVED + (uint64_t)f * fatsz, fat0, sizeof fat0);
    // the root directory, holding just the volume label
    uint8_t *root = calloc(spc, SECTOR);
    if (!root)
        return 1;
    memcpy(root, vol, 11);
    root[11] = 0x08;
    err |= put(data, root, (size_t)spc * SECTOR);
    close(dev);
    if (err) {
        dprintf(2, "mkfs.fat: write failed\n");
        return 1;
    }
    sync();
    printf("mkfs.fat: %s, %u clusters of %u bytes, label %.11s\n", path, clusters, spc * SECTOR, vol);
    return 0;
}
