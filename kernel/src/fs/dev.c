#include "fs/vfs.h"
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

static void add(const char *path, struct vnode *v, uint32_t mode, uint32_t gid)
{
    v->ino = vfs_next_ino();
    v->mode = mode;
    v->gid = gid;
    int err = vfs_mknod(path, v);
    if (err)
        pr_warn("dev: can't create %s (%d)\n", path, err);
}

void dev_init(void)
{
    // groups from /etc/group: input 20, video 21
    add("/dev/console", &tty_vnode, 0666, 0);
    add("/dev/null", &null_vnode, 0666, 0);
    add("/dev/zero", &zero_vnode, 0666, 0);
    add("/dev/tty", &devtty_vnode, 0666, 0);
    add("/dev/kbd", &kbd_vnode, 0660, 20);
    add("/dev/mouse", &mouse_vnode, 0660, 20);
    add("/dev/fb0", &fb_vnode, 0660, 21);
}
