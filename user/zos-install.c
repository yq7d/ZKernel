// zos-install: puts ZOS on a disk so it boots without the install media.
//
//   sudo zos-install                       asks everything
//   sudo zos-install --disk sda --user bob --password secret --hostname box --yes
//
// the disk gets a gpt table with an efi system partition (fat32, ZOSBOOT,
// holding limine, the kernel and the initrd) and the rest as ext2 (ZOSDATA,
// mounted on /data). booting needs uefi
#include "zk.h"

#define PAYLOAD  "/usr/share/zos/install"
#define BOOT_MB  256
#define SECTOR   512
#define ENTRIES  128

static const uint8_t esp_type[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };
static const uint8_t linux_type[16] = { 0xaf, 0x3d, 0xc6, 0x0f, 0x83, 0x84, 0x72, 0x47,
                                        0x8e, 0x79, 0x3d, 0x69, 0xd8, 0x47, 0x7d, 0xe4 };

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    dprintf(2, "\e[1;31mzos-install:\e[0m ");
    vdprintf(2, fmt, ap);
    dprintf(2, "\n");
    va_end(ap);
    exit(1);
}

static void step(const char *what)
{
    printf("\e[1;34m::\e[0m %s\n", what);
}

static uint32_t crc32(const void *data, size_t len)
{
    const uint8_t *p = data;
    uint32_t c = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320 & -(c & 1));
    }
    return ~c;
}

struct disk {
    char name[32];
    uint64_t bytes;
    char model[48];
};

static struct disk disks[16];
static int ndisks;

static void list_disks(void)
{
    int fd = open("/proc/disks", O_RDONLY);
    if (fd < 0)
        die("can't read /proc/disks");
    struct lreader *r = lr_open(fd);
    char *line;
    while (lr_getline(r, &line) >= 0 && ndisks < 16) {
        char *f[4] = { 0 }, *save;
        int n = 0;
        for (char *w = strtok_r(line, " ", &save); w; w = n < 2 ? strtok_r(NULL, " ", &save) : NULL)
            f[n++] = w;
        if (n == 2)
            f[n++] = strtok_r(NULL, " ", &save);
        if (n < 3 || line[0] == '#' || strcmp(f[2], "disk") != 0)
            continue;
        struct disk *d = &disks[ndisks++];
        strlcpy(d->name, f[0], sizeof d->name);
        d->bytes = (uint64_t)strtol(f[1], NULL, 10);
        strlcpy(d->model, save ? save : "", sizeof d->model);
    }
    lr_close(r);
    close(fd);
}

static void ask(const char *prompt, char *buf, size_t size, const char *def)
{
    printf("%s%s%s%s: ", prompt, def ? " [" : "", def ? def : "", def ? "]" : "");
    long n = readline(buf, size);
    if (n < 0)
        die("no input");
    if (!buf[0] && def)
        strlcpy(buf, def, size);
}

static int run(char **argv)
{
    long pid = spawn(argv[0], argv, NULL);
    if (pid < 0)
        return (int)pid;
    int status = -1;
    wait(pid, &status, 0);
    return status;
}

static void write_gpt(const char *disk, uint64_t sectors)
{
    int fd = open(disk, O_RDWR);
    if (fd < 0)
        die("%s: %s", disk, strerror(fd));
    uint64_t entry_secs = ENTRIES * 128 / SECTOR;    // 32
    uint64_t first_usable = 2 + entry_secs, last_usable = sectors - 2 - entry_secs;
    uint64_t boot_start = 2048, boot_end = boot_start + BOOT_MB * 2048ULL - 1;
    uint64_t data_start = boot_end + 1;
    uint64_t data_end = (last_usable + 1) / 2048 * 2048 - 1;    // keep it 1 MiB aligned
    if (data_end <= data_start + 2048 * 256)
        die("the disk is too small, ZOS needs at least 1 GiB");

    uint8_t *entries = calloc(ENTRIES, 128);
    struct { const uint8_t *type; uint64_t first, last; const char *name; } parts[2] = {
        { esp_type, boot_start, boot_end, "ZOS boot" },
        { linux_type, data_start, data_end, "ZOS data" },
    };
    for (int i = 0; i < 2; i++) {
        uint8_t *e = entries + i * 128;
        memcpy(e, parts[i].type, 16);
        getrandom(e + 16, 16);
        e[16 + 7] = (uint8_t)((e[16 + 7] & 0x0F) | 0x40);    // version 4 guid
        e[16 + 8] = (uint8_t)((e[16 + 8] & 0x3F) | 0x80);
        *(uint64_t *)(e + 32) = parts[i].first;
        *(uint64_t *)(e + 40) = parts[i].last;
        for (int k = 0; parts[i].name[k]; k++)
            e[56 + k * 2] = (uint8_t)parts[i].name[k];
    }
    uint32_t ecrc = crc32(entries, ENTRIES * 128);

    // the protective mbr says the whole disk is taken
    uint8_t mbr[SECTOR];
    memset(mbr, 0, sizeof mbr);
    uint8_t *pe = mbr + 446;
    pe[1] = 0x00; pe[2] = 0x02; pe[3] = 0x00;
    pe[4] = 0xEE;
    pe[5] = 0xFF; pe[6] = 0xFF; pe[7] = 0xFF;
    *(uint32_t *)(pe + 8) = 1;
    *(uint32_t *)(pe + 12) = sectors - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : (uint32_t)(sectors - 1);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint8_t primary[SECTOR], backup[SECTOR];
    uint8_t guid[16];
    getrandom(guid, 16);
    for (int which = 0; which < 2; which++) {
        uint8_t *h = which ? backup : primary;
        memset(h, 0, SECTOR);
        memcpy(h, "EFI PART", 8);
        *(uint32_t *)(h + 8) = 0x00010000;
        *(uint32_t *)(h + 12) = 92;
        *(uint64_t *)(h + 24) = which ? sectors - 1 : 1;
        *(uint64_t *)(h + 32) = which ? 1 : sectors - 1;
        *(uint64_t *)(h + 40) = first_usable;
        *(uint64_t *)(h + 48) = last_usable;
        memcpy(h + 56, guid, 16);
        *(uint64_t *)(h + 72) = which ? sectors - 1 - entry_secs : 2;
        *(uint32_t *)(h + 80) = ENTRIES;
        *(uint32_t *)(h + 84) = 128;
        *(uint32_t *)(h + 88) = ecrc;
        *(uint32_t *)(h + 16) = crc32(h, 92);
    }

    struct { uint64_t at; const void *data; size_t len; } writes[] = {
        { 0, mbr, SECTOR },
        { 1, primary, SECTOR },
        { 2, entries, ENTRIES * 128 },
        { sectors - 1 - entry_secs, entries, ENTRIES * 128 },
        { sectors - 1, backup, SECTOR },
    };
    for (size_t i = 0; i < sizeof writes / sizeof writes[0]; i++) {
        if (seek(fd, (long)(writes[i].at * SECTOR), SEEK_SET) < 0 ||
            write_all(fd, writes[i].data, writes[i].len) < 0)
            die("writing the partition table failed");
    }
    sync();
    int err = ioctl(fd, BLKRRPART, 0);
    close(fd);
    if (err)
        die("the kernel couldn't reread %s: %s", disk, strerror(err));
    free(entries);
}

static int copy(const char *from, const char *to, uint32_t mode)
{
    int in = open(from, O_RDONLY);
    if (in < 0)
        return in;
    int out = open(to, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return out;
    }
    static char buf[64 * 1024];
    long n;
    int err = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write_all(out, buf, (size_t)n) < 0) {
            err = ZK_EIO;
            break;
        }
    close(in);
    close(out);
    chmod(to, mode);
    return err;
}

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

static void write_text(const char *path, const char *text, uint32_t mode)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        die("%s: %s", path, strerror(fd));
    write_all(fd, text, strlen(text));
    close(fd);
    chmod(path, mode);
}

static bool valid_name(const char *s)
{
    if (!*s || strlen(s) > 31 || !islower(*s))
        return false;
    for (; *s; s++)
        if (!islower(*s) && !isdigit(*s) && *s != '-' && *s != '_')
            return false;
    return true;
}

int main(int argc, char **argv)
{
    char disk[32] = "", user[32] = "", password[128] = "", hostname[64] = "", yes = 0;
    bool have_password = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--disk") == 0 && i + 1 < argc)
            strlcpy(disk, basename_of(argv[++i]), sizeof disk);
        else if (strcmp(argv[i], "--user") == 0 && i + 1 < argc)
            strlcpy(user, argv[++i], sizeof user);
        else if (strcmp(argv[i], "--password") == 0 && i + 1 < argc) {
            strlcpy(password, argv[++i], sizeof password);
            have_password = true;
        } else if (strcmp(argv[i], "--hostname") == 0 && i + 1 < argc)
            strlcpy(hostname, argv[++i], sizeof hostname);
        else if (strcmp(argv[i], "--yes") == 0)
            yes = 1;
        else {
            printf("usage: zos-install [--disk NAME] [--user NAME] [--password PW] "
                   "[--hostname NAME] [--yes]\n");
            return 2;
        }
    }
    if (geteuid() != 0)
        die("this needs root: sudo zos-install");
    struct zk_stat st;
    if (stat(PAYLOAD "/zkernel", &st) != 0)
        die("no install files in " PAYLOAD ", this only works from the install media");

    printf("\n\e[1mZOS installer\e[0m\n\n");
    list_disks();
    if (!ndisks)
        die("no disks found. sata (ahci), nvme and virtio disks are supported");
    printf("disks:\n");
    for (int i = 0; i < ndisks; i++) {
        char size[16];
        format_size(disks[i].bytes, size, sizeof size);
        printf("  %-10s %8s  %s\n", disks[i].name, size, disks[i].model);
    }
    printf("\n");
    if (!disk[0])
        ask("install to which disk", disk, sizeof disk, ndisks == 1 ? disks[0].name : NULL);
    struct disk *target = NULL;
    for (int i = 0; i < ndisks; i++)
        if (strcmp(disks[i].name, disk) == 0)
            target = &disks[i];
    if (!target)
        die("no disk called %s", disk);
    if (target->bytes < (1ULL << 30))
        die("%s is too small, ZOS needs at least 1 GiB", disk);

    if (!user[0])
        ask("your user name", user, sizeof user, "zos");
    if (!valid_name(user))
        die("user names are lowercase letters, digits, - and _, starting with a letter");
    if (!have_password) {
        char again[128];
        for (;;) {
            read_password("password: ", password, sizeof password);
            read_password("password again: ", again, sizeof again);
            if (strcmp(password, again) == 0 && strlen(password) >= 3)
                break;
            printf("they don't match or it's shorter than 3 characters, try again\n");
        }
    }
    if (!hostname[0])
        ask("name for this computer", hostname, sizeof hostname, "zos");

    char dev[48];
    snprintf(dev, sizeof dev, "/dev/%s", disk);
    if (!yes) {
        char answer[16];
        printf("\n\e[1;31meverything on %s (%s) will be erased.\e[0m type yes to go on: ", dev,
               target->model);
        if (readline(answer, sizeof answer) < 0 || strcmp(answer, "yes") != 0)
            die("nothing was changed");
    }

    // partition device names: sda1 or nvme0n1p1
    size_t dl = strlen(disk);
    const char *sep = isdigit(disk[dl - 1]) ? "p" : "";
    char boot_dev[48], data_dev[48];
    snprintf(boot_dev, sizeof boot_dev, "/dev/%s%s1", disk, sep);
    snprintf(data_dev, sizeof data_dev, "/dev/%s%s2", disk, sep);

    step("writing the partition table");
    write_gpt(dev, target->bytes / SECTOR);

    step("making the boot filesystem");
    char *mkfat[] = { "/bin/mkfs.fat", "-n", "ZOSBOOT", boot_dev, NULL };
    if (run(mkfat) != 0)
        die("mkfs.fat failed");
    step("making the data filesystem");
    char *mkext[] = { "/bin/mkfs.ext2", "-L", "ZOSDATA", data_dev, NULL };
    if (run(mkext) != 0)
        die("mkfs.ext2 failed");

    const char *bm = "/tmp/zos-boot", *dm = "/tmp/zos-data";
    mkdir(bm);
    mkdir(dm);
    int err = mount(boot_dev, bm, "fat", "");
    if (!err)
        err = mount(data_dev, dm, "ext2", "");
    if (err)
        die("can't mount the new filesystems: %s", strerror(err));

    step("copying the boot loader, kernel and system");
    mkdirs("/tmp/zos-boot/EFI/BOOT");
    mkdirs("/tmp/zos-boot/boot/limine");
    if (copy(PAYLOAD "/BOOTX64.EFI", "/tmp/zos-boot/EFI/BOOT/BOOTX64.EFI", 0644) ||
        copy(PAYLOAD "/zkernel", "/tmp/zos-boot/boot/zkernel", 0644) ||
        copy(PAYLOAD "/initrd.tar", "/tmp/zos-boot/boot/initrd.tar", 0644))
        die("copying the boot files failed");
    write_text("/tmp/zos-boot/boot/limine/limine.conf",
               "timeout: 2\n\n/ZOS\n    protocol: limine\n    path: boot():/boot/zkernel\n"
               "    module_path: boot():/boot/initrd.tar\n", 0644);

    step("setting up your account");
    mkdirs("/tmp/zos-data/home");
    mkdirs("/tmp/zos-data/linux");
    mkdirs("/tmp/zos-data/pkg");
    mkdirs("/tmp/zos-data/local/etc");
    chmod("/tmp/zos-data/local/etc", 0755);
    char buf[1024];
    snprintf(buf, sizeof buf, "root:x:0:0:root:/root:/bin/sh\n%s:x:1000:1000:%s:/home/%s:/bin/sh\n",
             user, user, user);
    write_text("/tmp/zos-data/local/etc/passwd", buf, 0644);
    snprintf(buf, sizeof buf, "root:x:0:root\ndisk:x:6:root\nwheel:x:10:%s\ninput:x:20:%s\n"
             "video:x:21:%s\n%s:x:1000:\n", user, user, user, user);
    write_text("/tmp/zos-data/local/etc/group", buf, 0644);
    uint8_t raw[8];
    char salt[17], hash[128], root_hash[128];
    getrandom(raw, sizeof raw);
    for (int i = 0; i < 8; i++)
        snprintf(salt + i * 2, 3, "%02x", raw[i]);
    hash_password(password, salt, hash, sizeof hash);
    // root gets the same password, change it later with sudo passwd root
    getrandom(raw, sizeof raw);
    for (int i = 0; i < 8; i++)
        snprintf(salt + i * 2, 3, "%02x", raw[i]);
    hash_password(password, salt, root_hash, sizeof root_hash);
    memset(password, 0, sizeof password);
    snprintf(buf, sizeof buf, "root:%s\n%s:%s\n", root_hash, user, hash);
    write_text("/tmp/zos-data/local/etc/shadow", buf, 0600);
    snprintf(buf, sizeof buf, "%s\n", hostname);
    write_text("/tmp/zos-data/local/etc/hostname", buf, 0644);
    snprintf(buf, sizeof buf, "%s\n", user);
    write_text("/tmp/zos-data/local/etc/autologin", buf, 0644);

    step("finishing up");
    sync();
    umount(bm);
    umount(dm);
    sync();
    printf("\n\e[1;32mZOS is installed on %s.\e[0m take out the install media and reboot\n"
           "(the firmware has to boot in uefi mode)\n", dev);
    return 0;
}
