#include "drivers/blk.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define BLOCK      4096
#define SPB        (BLOCK / SECTOR_SIZE)
#define CACHE_SIZE 256
#define MAX_DEVS   8

struct cblock {
    struct blkdev *dev;
    uint64_t block;
    uint64_t used;    // lru stamp, 0 = empty
    uint8_t *data;
};

static struct cblock cache[CACHE_SIZE];
static uint64_t stamp;
static struct mutex lock = MUTEX_INIT;
static struct blkdev *devs[MAX_DEVS];
static int ndevs;
static struct blk_stats stats;

// sectors in this block, the last one can be short
static uint32_t block_sectors(struct blkdev *d, uint64_t block)
{
    uint64_t lba = block * SPB;
    return d->sectors - lba < SPB ? (uint32_t)(d->sectors - lba) : SPB;
}

// lock held
static struct cblock *get_block(struct blkdev *d, uint64_t block, bool fill, int *err)
{
    struct cblock *victim = NULL;
    for (int i = 0; i < CACHE_SIZE; i++) {
        struct cblock *c = &cache[i];
        if (c->used && c->dev == d && c->block == block) {
            c->used = ++stamp;
            stats.hits++;
            return c;
        }
        if (!victim || c->used < victim->used)
            victim = c;
    }
    stats.misses++;
    if (!victim->data) {
        victim->data = kmalloc(BLOCK);
        if (!victim->data) {
            *err = ZK_ENOMEM;
            return NULL;
        }
    }
    victim->used = 0;
    if (fill) {
        *err = d->read(d, block * SPB, block_sectors(d, block), victim->data);
        if (*err)
            return NULL;
    }
    victim->dev = d;
    victim->block = block;
    victim->used = ++stamp;
    return victim;
}

static int rw(struct blkdev *d, uint64_t off, uint8_t *buf, size_t len, bool write)
{
    uint64_t size = d->sectors * SECTOR_SIZE;
    if (off > size || len > size - off)
        return ZK_EINVAL;

    mutex_lock(&lock);
    int err = 0;
    while (len && !err) {
        uint64_t block = off / BLOCK;
        size_t in = off % BLOCK;
        size_t n = BLOCK - in < len ? BLOCK - in : len;
        // a whole block overwrite doesn't need the old contents
        bool fill = !write || n != BLOCK;
        struct cblock *c = get_block(d, block, fill, &err);
        if (!c)
            break;
        if (write) {
            memcpy(c->data + in, buf, n);
            // write through, only the sectors that changed
            uint64_t first = in / SECTOR_SIZE, last = (in + n - 1) / SECTOR_SIZE;
            err = d->write(d, block * SPB + first, (uint32_t)(last - first + 1),
                           c->data + first * SECTOR_SIZE);
            stats.writes++;
            if (err)
                c->used = 0;    // contents unknown now
        } else {
            memcpy(buf, c->data + in, n);
        }
        off += n;
        buf += n;
        len -= n;
    }
    mutex_unlock(&lock);
    return err;
}

int blk_read(struct blkdev *d, uint64_t off, void *buf, size_t len)
{
    return rw(d, off, buf, len, false);
}

int blk_write(struct blkdev *d, uint64_t off, const void *buf, size_t len)
{
    return rw(d, off, (uint8_t *)buf, len, true);
}

void blk_invalidate(struct blkdev *d)
{
    mutex_lock(&lock);
    for (int i = 0; i < CACHE_SIZE; i++)
        if (cache[i].dev == d)
            cache[i].used = 0;
    mutex_unlock(&lock);
}

void blk_get_stats(struct blk_stats *s)
{
    *s = stats;
}

static int64_t dev_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct blkdev *d = v->priv;
    uint64_t size = d->sectors * SECTOR_SIZE;
    if (off >= size)
        return 0;
    if (len > size - off)
        len = size - off;
    int err = blk_read(d, off, buf, len);
    return err ? err : (int64_t)len;
}

static int64_t dev_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    struct blkdev *d = v->priv;
    uint64_t size = d->sectors * SECTOR_SIZE;
    if (off >= size)
        return len ? ZK_ENOSPC : 0;
    if (len > size - off)
        len = size - off;
    int err = blk_write(d, off, buf, len);
    return err ? err : (int64_t)len;
}

static const struct vnode_ops blk_ops = { .read = dev_read, .write = dev_write };

void blk_register(struct blkdev *d)
{
    if (ndevs == MAX_DEVS) {
        pr_warn("blk: too many devices, ignoring %s\n", d->name);
        return;
    }
    devs[ndevs++] = d;
    d->node = (struct vnode){ .type = VNODE_DEV, .refs = 1, .ino = vfs_next_ino(),
                              .size = d->sectors * SECTOR_SIZE, .ops = &blk_ops, .priv = d };
    char path[32];
    snprintf(path, sizeof path, "/dev/%s", d->name);
    int err = vfs_mknod(path, &d->node);
    if (err)
        pr_warn("blk: can't create %s (%d)\n", path, err);
}

struct blkdev *blk_find(const char *path)
{
    struct vnode *v;
    if (vfs_lookup(path, &v) != 0)
        return NULL;
    struct blkdev *d = v->ops == &blk_ops ? v->priv : NULL;
    vnode_put(v);
    return d;
}

int blk_count(void)
{
    return ndevs;
}

struct blkdev *blk_get(int i)
{
    return i >= 0 && i < ndevs ? devs[i] : NULL;
}
