#include "fs/vfs.h"
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

static void add(const char *path, struct vnode *v)
{
    v->ino = vfs_next_ino();
    int err = vfs_mknod(path, v);
    if (err)
        pr_warn("dev: can't create %s (%d)\n", path, err);
}

void dev_init(void)
{
    add("/dev/console", &tty_vnode);
    add("/dev/null", &null_vnode);
    add("/dev/zero", &zero_vnode);
}
