// mkfs.ext2 [-L LABEL] /dev/xxx: a fresh ext2 filesystem, 4k blocks, sparse
// superblock backups, one inode per 64 KiB
#include "zk.h"

#define BS        4096
#define BPG       32768
#define INODE     128
#define BYTES_PER_INODE (64 * 1024)
#define ROOT_INO  2
#define LF_INO    11

static int dev;
static uint8_t blk[BS];

static int put(uint64_t block, const void *buf)
{
    if (seek(dev, (long)(block * BS), SEEK_SET) < 0)
        return ZK_EIO;
    return write_all(dev, buf, BS) < 0 ? ZK_EIO : 0;
}

static bool is_power(uint32_t n, uint32_t base)
{
    while (n > 1 && n % base == 0)
        n /= base;
    return n == 1;
}

static bool has_super(uint32_t g)
{
    return g <= 1 || is_power(g, 3) || is_power(g, 5) || is_power(g, 7);
}

static uint64_t device_bytes(const char *path)
{
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
    return bytes;
}

static void set_bit(uint8_t *map, uint32_t bit)
{
    map[bit / 8] |= (uint8_t)(1 << (bit % 8));
}

struct inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint8_t osd2[12];
};

static void dirent(uint8_t *at, uint32_t ino, uint16_t rec, const char *name, uint8_t type)
{
    *(uint32_t *)at = ino;
    *(uint16_t *)(at + 4) = rec;
    at[6] = (uint8_t)strlen(name);
    at[7] = type;
    memcpy(at + 8, name, strlen(name));
}

int main(int argc, char **argv)
{
    const char *label = "", *path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-L") == 0 && i + 1 < argc)
            label = argv[++i];
        else
            path = argv[i];
    }
    if (!path) {
        dprintf(2, "usage: mkfs.ext2 [-L LABEL] /dev/device\n");
        return 2;
    }
    uint64_t blocks64 = device_bytes(path) / BS;
    if (blocks64 < 2048) {
        dprintf(2, "mkfs.ext2: %s is too small\n", path);
        return 1;
    }
    if (blocks64 > 0xFFFFFFFFUL)
        blocks64 = 0xFFFFFFFFUL;
    uint32_t blocks = (uint32_t)blocks64;
    uint32_t groups = (blocks + BPG - 1) / BPG;
    uint32_t ipg = (uint32_t)((uint64_t)BPG * BS / BYTES_PER_INODE);
    uint32_t itb = ipg * INODE / BS;
    uint32_t gdt_blocks = (groups * 32 + BS - 1) / BS;
    // a tiny last group can't hold its own metadata, drop it
    uint32_t last = blocks - (groups - 1) * BPG;
    if (groups > 1 && last < (has_super(groups - 1) ? 1 + gdt_blocks : 0) + 2 + itb + 16) {
        groups--;
        blocks = groups * BPG;
        gdt_blocks = (groups * 32 + BS - 1) / BS;
    }

    dev = open(path, O_RDWR);
    if (dev < 0) {
        dprintf(2, "mkfs.ext2: %s: %s\n", path, strerror(dev));
        return 1;
    }
    uint8_t *gdt = calloc(gdt_blocks, BS);
    if (!gdt)
        return 1;
    uint32_t now = (uint32_t)time();
    uint32_t free_total = 0, root_block = 0, lf_block = 0;

    for (uint32_t g = 0; g < groups; g++) {
        uint32_t start = g * BPG;
        uint32_t in_group = g == groups - 1 ? blocks - start : BPG;
        uint32_t meta = (has_super(g) ? 1 + gdt_blocks : 0);
        uint32_t bbitmap = start + meta, ibitmap = bbitmap + 1, itable = ibitmap + 1;
        uint32_t used = meta + 2 + itb;
        uint32_t dirs = 0;
        if (g == 0) {
            root_block = start + used;
            lf_block = root_block + 1;
            used += 2;
            dirs = 2;
        }

        // block bitmap: metadata used, the rest free, padding past the end set
        memset(blk, 0, BS);
        for (uint32_t b = 0; b < used; b++)
            set_bit(blk, b);
        for (uint32_t b = in_group; b < BS * 8; b++)
            set_bit(blk, b);
        if (put(bbitmap, blk))
            goto io;
        // inode bitmap: the reserved ones in group 0
        memset(blk, 0, BS);
        if (g == 0)
            for (uint32_t i = 0; i < LF_INO; i++)
                set_bit(blk, i);
        for (uint32_t i = ipg; i < BS * 8; i++)
            set_bit(blk, i);
        if (put(ibitmap, blk))
            goto io;
        memset(blk, 0, BS);
        for (uint32_t b = 0; b < itb; b++)
            if (put(itable + b, blk))
                goto io;

        uint8_t *d = gdt + (size_t)g * 32;
        *(uint32_t *)d = bbitmap;
        *(uint32_t *)(d + 4) = ibitmap;
        *(uint32_t *)(d + 8) = itable;
        *(uint16_t *)(d + 12) = (uint16_t)(in_group - used);
        *(uint16_t *)(d + 14) = (uint16_t)(g == 0 ? ipg - LF_INO : ipg);
        *(uint16_t *)(d + 16) = (uint16_t)dirs;
        free_total += in_group - used;
        if (g == 0)
            printf("\rmkfs.ext2: writing group 1/%u", groups);
        else if (g % 16 == 0 || g == groups - 1)
            printf("\rmkfs.ext2: writing group %u/%u", g + 1, groups);
    }
    printf("\n");

    // root and lost+found
    struct inode in;
    uint32_t itable0 = *(uint32_t *)(gdt + 8);
    memset(&in, 0, sizeof in);
    in.mode = 040755;
    in.size = BS;
    in.atime = in.ctime = in.mtime = now;
    in.links = 3;
    in.blocks = BS / 512;
    in.block[0] = root_block;
    uint8_t itbuf[BS];
    memset(itbuf, 0, BS);
    memcpy(itbuf + (ROOT_INO - 1) * INODE, &in, sizeof in);
    in.mode = 040700;
    in.links = 2;
    in.block[0] = lf_block;
    memcpy(itbuf + (LF_INO - 1) * INODE, &in, sizeof in);
    if (put(itable0, itbuf))
        goto io;
    memset(blk, 0, BS);
    dirent(blk, ROOT_INO, 12, ".", 2);
    dirent(blk + 12, ROOT_INO, 12, "..", 2);
    dirent(blk + 24, LF_INO, BS - 24, "lost+found", 2);
    if (put(root_block, blk))
        goto io;
    memset(blk, 0, BS);
    dirent(blk, LF_INO, 12, ".", 2);
    dirent(blk + 12, ROOT_INO, BS - 12, "..", 2);
    if (put(lf_block, blk))
        goto io;

    // superblock, plus a backup and the group descriptors in the sparse groups
    uint8_t sb[1024];
    memset(sb, 0, sizeof sb);
    uint32_t *w = (uint32_t *)sb;
    w[0] = groups * ipg;
    w[1] = blocks;
    w[2] = blocks / 20;    // reserved for root
    w[3] = free_total;
    w[4] = groups * ipg - LF_INO;
    w[5] = 0;              // first data block, 0 with 4k blocks
    w[6] = 2;              // log2(block size) - 10
    w[7] = 2;
    w[8] = BPG;
    w[9] = BPG;
    w[10] = ipg;
    w[12] = now;
    *(uint16_t *)(sb + 54) = 0xFFFF;    // max mount count, never forced
    *(uint16_t *)(sb + 56) = 0xEF53;
    *(uint16_t *)(sb + 58) = 1;         // clean
    *(uint16_t *)(sb + 60) = 1;         // on errors continue
    w[16] = now;
    w[19] = 1;                          // dynamic revision
    w[21] = LF_INO;
    *(uint16_t *)(sb + 88) = INODE;
    w[24] = 0x0002;                     // incompat: file type in dir entries
    w[25] = 0x0001 | 0x0002;            // ro compat: sparse super, large files
    getrandom(sb + 104, 16);            // uuid
    strlcpy((char *)sb + 120, label, 16);
    for (uint32_t g = 0; g < groups; g++) {
        if (!has_super(g))
            continue;
        uint64_t start = (uint64_t)g * BPG;
        memset(blk, 0, BS);
        *(uint16_t *)(sb + 90) = (uint16_t)g;    // which group this copy is in
        memcpy(blk + (g == 0 ? 1024 : 0), sb, sizeof sb);
        if (put(start, blk))
            goto io;
        for (uint32_t b = 0; b < gdt_blocks; b++)
            if (put(start + 1 + b, gdt + (size_t)b * BS))
                goto io;
    }
    close(dev);
    sync();
    printf("mkfs.ext2: %s, %u blocks of 4k, %u inodes, label %s\n", path, blocks, groups * ipg,
           label[0] ? label : "(none)");
    return 0;
io:
    dprintf(2, "mkfs.ext2: write failed\n");
    return 1;
}
