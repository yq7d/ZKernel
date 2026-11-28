#include "fs/vfs.h"
#include "drivers/evdev.h"
#include "drivers/fbdev.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/tty.h"
#include "kprintf.h"
#include "lib/string.h"

static int64_t null_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)v; (void)buf; (void)len; (void)off;
    return 0;
}

static int64_t null_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)v; (void)buf; (void)off;
    return (int64_t)len;
}

static int64_t zero_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)v; (void)off;
    memset(buf, 0, len);
    return (int64_t)len;
}

static const struct vnode_ops null_ops = { .read = null_read, .write = null_write };
static const struct vnode_ops zero_ops = { .read = zero_read, .write = null_write };

static struct vnode null_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &null_ops };
static struct vnode zero_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &zero_ops };

// linux device numbers, by inode, for stat
static struct { uint64_t ino; uint32_t rdev; } rdevs[16];
static int nrdevs;

uint32_t dev_rdev(uint64_t ino)
{
    for (int i = 0; i < nrdevs; i++)
        if (rdevs[i].ino == ino)
            return rdevs[i].rdev;
    return 0;
}

static void add(const char *path, struct vnode *v, uint32_t mode, uint32_t gid,
                int major, int minor)
{
    v->ino = vfs_next_ino();
    if (nrdevs < 16)
        rdevs[nrdevs++] = (typeof(rdevs[0])){ v->ino, (uint32_t)(major << 8 | minor) };
    v->mode = mode;
    v->gid = gid;
    int err = vfs_mknod(path, v);
    if (err)
        pr_warn("dev: can't create %s (%d)\n", path, err);
}

void dev_init(void)
{
    // groups from /etc/group: input 20, video 21
    add("/dev/console", &tty_vnode, 0666, 0, 5, 1);
    add("/dev/null", &null_vnode, 0666, 0, 1, 3);
    add("/dev/zero", &zero_vnode, 0666, 0, 1, 5);
    add("/dev/tty", &devtty_vnode, 0666, 0, 5, 0);
    add("/dev/kbd", &kbd_vnode, 0660, 20, 10, 200);
    add("/dev/mouse", &mouse_vnode, 0660, 20, 10, 201);
    add("/dev/fb0", &fb_vnode, 0660, 21, 29, 0);

    vfs_mkdir("/dev/pts");
    add("/dev/ptmx", &ptmx_vnode, 0666, 0, 5, 2);

    // linux names: input devices and vts, every vt is the console
    evdev_init();
    vfs_mkdir("/dev/input");
    add("/dev/input/event0", &evdev_kbd.vnode, 0660, 20, 13, 64);
    add("/dev/input/event1", &evdev_mouse.vnode, 0660, 20, 13, 65);
    static const char *const vts[] = {
        "/dev/tty0", "/dev/tty1", "/dev/tty2", "/dev/tty3", "/dev/tty4", "/dev/tty5", "/dev/tty6",
    };
    for (unsigned i = 0; i < sizeof vts / sizeof vts[0]; i++)
        if (vfs_mknod(vts[i], &tty_vnode))
            pr_warn("dev: can't create %s\n", vts[i]);
}
