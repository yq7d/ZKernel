// /sys, laid out like linux's: a tmpfs filled in once at boot, the devices
// don't change after that
#include "fs/sysfs.h"
#include "fs/vfs.h"
#include "drivers/evdev.h"
#include "drivers/fb.h"
#include "drivers/pci.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include <stdarg.h>

static int sysfs_mount(struct mount *m, const char *source, const char *opts)
{
    (void)opts;
    return ramfs_type.mount(m, source, "mode=755");
}

const struct fs_type sysfs_type = { "sysfs", sysfs_mount };

static void mkdirs(const char *path)
{
    char buf[ZK_PATH_MAX];
    strlcpy(buf, path, sizeof buf);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        vfs_mkdir(buf);
        *p = '/';
    }
    vfs_mkdir(buf);
}

static void put_raw(const char *path, const void *data, size_t len)
{
    char dir[ZK_PATH_MAX];
    strlcpy(dir, path, sizeof dir);
    char *slash = strrchr(dir, '/');
    *slash = '\0';
    mkdirs(dir);
    struct file *f;
    if (vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, &f)) {
        pr_warn("sysfs: can't write %s\n", path);
        return;
    }
    file_write(f, data, len);
    file_put(f);
    vfs_chmod(path, 0444);
}

__attribute__((format(printf, 2, 3)))
static void put(const char *path, const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    put_raw(path, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

static void slink(const char *target, const char *path)
{
    char dir[ZK_PATH_MAX];
    strlcpy(dir, path, sizeof dir);
    *strrchr(dir, '/') = '\0';
    mkdirs(dir);
    vfs_symlink(target, path);
}

// a bitmap the way linux prints them: 64 bit words in hex, top first,
// leading zero words left out
static void bitmap(char *out, size_t size, const uint8_t *bits, size_t nbytes)
{
    int words = (int)((nbytes + 7) / 8);
    int top = words - 1;
    uint64_t w[16] = { 0 };
    for (size_t i = 0; i < nbytes; i++)
        w[i / 8] |= (uint64_t)bits[i] << (8 * (i % 8));
    while (top > 0 && !w[top])
        top--;
    size_t len = 0;
    for (int i = top; i >= 0; i--)
        len += (size_t)snprintf(out + len, size - len, i == top ? "%lx" : " %lx", w[i]);
    snprintf(out + len, size - len, "\n");
}

static void input_dev(struct evdev *d, int n, const char *serio)
{
    char base[160], p[200], tmp[256];
    snprintf(base, sizeof base, "/sys/devices/platform/i8042/%s/input/input%d", serio, n);

    snprintf(p, sizeof p, "%s/name", base);
    put(p, "%s\n", d->name);
    snprintf(p, sizeof p, "%s/phys", base);
    put(p, "isa0060/%s/input0\n", serio);
    snprintf(p, sizeof p, "%s/uniq", base);
    put(p, "\n");
    snprintf(p, sizeof p, "%s/properties", base);
    put(p, "0\n");
    snprintf(p, sizeof p, "%s/id/bustype", base);
    put(p, "0011\n");
    snprintf(p, sizeof p, "%s/id/vendor", base);
    put(p, "0001\n");
    snprintf(p, sizeof p, "%s/id/product", base);
    put(p, "%04x\n", d->product);
    snprintf(p, sizeof p, "%s/id/version", base);
    put(p, "ab41\n");

    bitmap(tmp, sizeof tmp, d->evbits, sizeof d->evbits);
    snprintf(p, sizeof p, "%s/capabilities/ev", base);
    put(p, "%s", tmp);
    bitmap(tmp, sizeof tmp, d->keybits, sizeof d->keybits);
    snprintf(p, sizeof p, "%s/capabilities/key", base);
    put(p, "%s", tmp);
    bitmap(tmp, sizeof tmp, d->relbits, sizeof d->relbits);
    snprintf(p, sizeof p, "%s/capabilities/rel", base);
    put(p, "%s", tmp);
    static const char *const empty[] = { "abs", "msc", "led", "snd", "sw", "ff" };
    for (unsigned i = 0; i < sizeof empty / sizeof empty[0]; i++) {
        snprintf(p, sizeof p, "%s/capabilities/%s", base, empty[i]);
        put(p, "0\n");
    }
    snprintf(p, sizeof p, "%s/uevent", base);
    put(p, "PRODUCT=11/1/%x/ab41\nNAME=\"%s\"\nPHYS=\"isa0060/%s/input0\"\n", d->product,
        d->name, serio);
    snprintf(p, sizeof p, "%s/subsystem", base);
    slink("../../../../../../class/input", p);

    // the event node under it
    snprintf(p, sizeof p, "%s/event%d/dev", base, n);
    put(p, "13:%d\n", 64 + n);
    snprintf(p, sizeof p, "%s/event%d/uevent", base, n);
    put(p, "MAJOR=13\nMINOR=%d\nDEVNAME=input/event%d\n", 64 + n, n);
    snprintf(p, sizeof p, "%s/event%d/device", base, n);
    snprintf(tmp, sizeof tmp, "../../input%d", n);
    slink(tmp, p);
    snprintf(p, sizeof p, "%s/event%d/subsystem", base, n);
    slink("../../../../../../../class/input", p);

    snprintf(tmp, sizeof tmp, "../../devices/platform/i8042/%s/input/input%d", serio, n);
    snprintf(p, sizeof p, "/sys/class/input/input%d", n);
    slink(tmp, p);
    snprintf(tmp, sizeof tmp, "../../devices/platform/i8042/%s/input/input%d/event%d", serio, n, n);
    snprintf(p, sizeof p, "/sys/class/input/event%d", n);
    slink(tmp, p);
    snprintf(p, sizeof p, "/sys/dev/char/13:%d", 64 + n);
    slink(tmp, p);
}

// under the vga card when there is one, like a real gpu's framebuffer. xorg's
// fbdev driver finds it through the pci device
static void framebuffer(void)
{
    char b[128], p[192], rel[160];
    const struct pci_dev *vga = NULL;
    for (int i = 0; i < pci_count() && !vga; i++)
        if (pci_get(i)->class == 3)
            vga = pci_get(i);
    if (vga)
        snprintf(b, sizeof b, "devices/pci0000:00/0000:%02x:%02x.%x/graphics/fb0",
                 vga->bus, vga->dev, vga->fn);
    else
        snprintf(b, sizeof b, "devices/platform/simple-framebuffer.0/graphics/fb0");

    snprintf(p, sizeof p, "/sys/%s/dev", b);
    put(p, "29:0\n");
    snprintf(p, sizeof p, "/sys/%s/uevent", b);
    put(p, "MAJOR=29\nMINOR=0\nDEVNAME=fb0\n");
    snprintf(p, sizeof p, "/sys/%s/name", b);
    put(p, "zkfb\n");
    snprintf(p, sizeof p, "/sys/%s/bits_per_pixel", b);
    put(p, "32\n");
    snprintf(p, sizeof p, "/sys/%s/virtual_size", b);
    put(p, "%u,%u\n", (unsigned)fb.width, (unsigned)fb.height);
    snprintf(p, sizeof p, "/sys/%s/stride", b);
    put(p, "%u\n", (unsigned)fb.pitch);
    snprintf(p, sizeof p, "/sys/%s/modes", b);
    put(p, "U:%ux%up-60\n", (unsigned)fb.width, (unsigned)fb.height);
    snprintf(p, sizeof p, "/sys/%s/subsystem", b);
    slink("../../../../../class/graphics", p);
    snprintf(p, sizeof p, "/sys/%s/device", b);
    slink("../..", p);
    snprintf(rel, sizeof rel, "../../%s", b);
    slink(rel, "/sys/class/graphics/fb0");
    slink(rel, "/sys/dev/char/29:0");
}

static void vts(void)
{
    char p[96];
    for (int i = 0; i <= 6; i++) {
        snprintf(p, sizeof p, "/sys/devices/virtual/tty/tty%d/dev", i);
        put(p, "4:%d\n", i);
        snprintf(p, sizeof p, "/sys/devices/virtual/tty/tty%d/uevent", i);
        put(p, "MAJOR=4\nMINOR=%d\nDEVNAME=tty%d\n", i, i);
        char t[64];
        snprintf(t, sizeof t, "../../devices/virtual/tty/tty%d", i);
        snprintf(p, sizeof p, "/sys/class/tty/tty%d", i);
        slink(t, p);
    }
    put("/sys/devices/virtual/tty/tty0/active", "tty1\n");
    put("/sys/devices/virtual/tty/console/active", "tty1\n");
    slink("../../devices/virtual/tty/console", "/sys/class/tty/console");
}

static void pci(void)
{
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        char name[16], b[96], p[128], t[96];
        snprintf(name, sizeof name, "0000:%02x:%02x.%x", d->bus, d->dev, d->fn);
        snprintf(b, sizeof b, "/sys/devices/pci0000:00/%s", name);

        snprintf(p, sizeof p, "%s/vendor", b);
        put(p, "0x%04x\n", d->vendor);
        snprintf(p, sizeof p, "%s/device", b);
        put(p, "0x%04x\n", d->device);
        snprintf(p, sizeof p, "%s/class", b);
        put(p, "0x%02x%02x%02x\n", d->class, d->subclass, d->prog_if);
        uint32_t sub = pci_read32(d, 0x2c);
        snprintf(p, sizeof p, "%s/subsystem_vendor", b);
        put(p, "0x%04x\n", sub & 0xffff);
        snprintf(p, sizeof p, "%s/subsystem_device", b);
        put(p, "0x%04x\n", sub >> 16);
        snprintf(p, sizeof p, "%s/revision", b);
        put(p, "0x%02x\n", pci_read32(d, 8) & 0xff);
        snprintf(p, sizeof p, "%s/irq", b);
        put(p, "%u\n", d->irq);
        if (d->class == 3) {
            snprintf(p, sizeof p, "%s/boot_vga", b);
            put(p, "1\n");
        }

        uint32_t cfg[64];
        for (int r = 0; r < 64; r++)
            cfg[r] = pci_read32(d, (uint8_t)(r * 4));
        snprintf(p, sizeof p, "%s/config", b);
        put_raw(p, cfg, sizeof cfg);

        // no bar sizes, the bars are in config for anyone who wants them
        char res[13 * 60];
        size_t n = 0;
        for (int r = 0; r < 13; r++)
            n += (size_t)snprintf(res + n, sizeof res - n,
                                  "0x%016x 0x%016x 0x%016x\n", 0, 0, 0);
        snprintf(p, sizeof p, "%s/resource", b);
        put_raw(p, res, n);

        snprintf(p, sizeof p, "%s/uevent", b);
        put(p, "PCI_CLASS=%X%02X%02X\nPCI_ID=%04X:%04X\nPCI_SLOT_NAME=%s\n",
            d->class, d->subclass, d->prog_if, d->vendor, d->device, name);
        snprintf(p, sizeof p, "%s/subsystem", b);
        slink("../../../bus/pci", p);
        snprintf(t, sizeof t, "../../../devices/pci0000:00/%s", name);
        snprintf(p, sizeof p, "/sys/bus/pci/devices/%s", name);
        slink(t, p);
    }
}

void sysfs_init(void)
{
    vfs_mkdir("/sys");
    if (vfs_mount("sysfs", "/sys", "sysfs", "")) {
        pr_warn("sysfs: can't mount /sys\n");
        return;
    }
    input_dev(&evdev_kbd, 0, "serio0");
    input_dev(&evdev_mouse, 1, "serio1");
    pci();
    framebuffer();
    vts();
    put("/sys/devices/system/cpu/online", "0\n");
    put("/sys/devices/system/cpu/possible", "0\n");
    put("/sys/devices/system/cpu/present", "0\n");
    mkdirs("/sys/devices/system/cpu/cpu0");
    mkdirs("/sys/class/drm");
    mkdirs("/sys/kernel");
    mkdirs("/sys/fs/cgroup");
    mkdirs("/sys/firmware");
    mkdirs("/sys/block");
    vfs_chmod("/sys", 0555);
    pr_ok("sysfs: /sys ready\n");
}
