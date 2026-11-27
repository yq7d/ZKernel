#include "drivers/blk.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sched.h"
#include "sched/sync.h"

#define BLOCK      4096
#define SPB        (BLOCK / SECTOR_SIZE)
#define CACHE_SIZE 4096    // 16 MiB once full
#define BUCKETS    1024
#define MAX_DEVS   32
#define FLUSH_MS   1000

// write-back: writes land here and a flusher thread pushes them out every
// second, or sooner on sync. a crash loses at most that much
struct cblock {
    struct blkdev *dev;
    uint64_t block;
    uint64_t used;    // lru stamp, 0 = empty
    uint8_t *data;
    bool dirty;
    struct cblock *hnext;
};

static struct cblock cache[CACHE_SIZE];
static struct cblock *buckets[BUCKETS];
static uint64_t stamp;
static struct mutex lock = MUTEX_INIT;
static struct blkdev *devs[MAX_DEVS];
static int ndevs;
static struct blk_stats stats;
static size_t clock_hand;

static size_t hash(struct blkdev *d, uint64_t block)
{
    return (size_t)(((uintptr_t)d >> 4) ^ block ^ (block >> 10)) % BUCKETS;
}

static void unhash(struct cblock *c)
{
    for (struct cblock **pp = &buckets[hash(c->dev, c->block)]; *pp; pp = &(*pp)->hnext) {
        if (*pp == c) {
            *pp = c->hnext;
            return;
        }
    }
}

// sectors in this block, the last one can be short
static uint32_t block_sectors(struct blkdev *d, uint64_t block)
{
    uint64_t lba = block * SPB;
    return d->sectors - lba < SPB ? (uint32_t)(d->sectors - lba) : SPB;
}

// lock held
static int flush_one(struct cblock *c)
{
    if (!c->dirty)
        return 0;
    int err = c->dev->write(c->dev, c->block * SPB, block_sectors(c->dev, c->block), c->data);
    stats.writes++;
    if (!err)
        c->dirty = false;
    return err;
}

// lock held
static struct cblock *get_block(struct blkdev *d, uint64_t block, bool fill, int *err)
{
    for (struct cblock *c = buckets[hash(d, block)]; c; c = c->hnext) {
        if (c->dev == d && c->block == block) {
            c->used = ++stamp;
            stats.hits++;
            return c;
        }
    }
    stats.misses++;
    // an empty slot, else the oldest of a handful (second chance style)
    struct cblock *victim = NULL;
    for (size_t n = 0; n < CACHE_SIZE && !victim; n++) {
        struct cblock *c = &cache[(clock_hand + n) % CACHE_SIZE];
        if (!c->used)
            victim = c;
    }
    if (!victim) {
        for (int n = 0; n < 32; n++) {
            struct cblock *c = &cache[clock_hand++ % CACHE_SIZE];
            if (!victim || c->used < victim->used)
                victim = c;
        }
    }
    if (victim->used) {
        *err = flush_one(victim);
        if (*err)
            return NULL;
        unhash(victim);
        victim->used = 0;
    }
    if (!victim->data) {
        victim->data = kmalloc(BLOCK);
        if (!victim->data) {
            *err = ZK_ENOMEM;
            return NULL;
        }
    }
    if (fill) {
        *err = d->read(d, block * SPB, block_sectors(d, block), victim->data);
        if (*err)
            return NULL;
    }
    victim->dev = d;
    victim->block = block;
    victim->dirty = false;
    victim->used = ++stamp;
    victim->hnext = buckets[hash(d, block)];
    buckets[hash(d, block)] = victim;
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
            c->dirty = true;
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

int blk_sync(void)
{
    int err = 0;
    mutex_lock(&lock);
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        int e = cache[i].used ? flush_one(&cache[i]) : 0;
        if (e && !err)
            err = e;
    }
    mutex_unlock(&lock);
    return err;
}

void blk_invalidate(struct blkdev *d)
{
    mutex_lock(&lock);
    for (size_t i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].used && cache[i].dev == d) {
            flush_one(&cache[i]);
            unhash(&cache[i]);
            cache[i].used = 0;
        }
    }
    mutex_unlock(&lock);
}

static void flusher(void *arg)
{
    (void)arg;
    for (;;) {
        thread_sleep_ms(FLUSH_MS);
        blk_sync();
    }
}

void blk_start_flusher(void)
{
    thread_create("flush", flusher, NULL, false);
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

static int dev_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)f; (void)arg;
    struct blkdev *d = v->priv;
    if (req == BLKRRPART)
        return d->is_part ? ZK_EINVAL : blk_rescan(d);
    return ZK_EINVAL;
}

static const struct vnode_ops blk_ops = { .read = dev_read, .write = dev_write, .ioctl = dev_ioctl };

void blk_register(struct blkdev *d)
{
    if (ndevs == MAX_DEVS) {
        pr_warn("blk: too many devices, ignoring %s\n", d->name);
        return;
    }
    devs[ndevs++] = d;
    // group disk, 6
    d->node = (struct vnode){ .type = VNODE_DEV, .refs = 1, .ino = vfs_next_ino(),
                              .size = d->sectors * SECTOR_SIZE, .ops = &blk_ops, .priv = d,
                              .mode = 0660, .gid = 6 };
    char path[32];
    snprintf(path, sizeof path, "/dev/%s", d->name);
    int err = vfs_mknod(path, &d->node);
    if (err)
        pr_warn("blk: can't create %s (%d)\n", path, err);
}

// partitions are devices of their own that add an offset
struct part {
    struct blkdev blk;
    struct blkdev *parent;
    uint64_t start;
};

static int part_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    struct part *p = d->priv;
    return p->parent->read(p->parent, p->start + lba, count, buf);
}

static int part_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct part *p = d->priv;
    return p->parent->write(p->parent, p->start + lba, count, buf);
}

static void add_part(struct blkdev *d, int n, uint64_t start, uint64_t count, const char *kind)
{
    if (!count || start + count > d->sectors)
        return;
    struct part *p = kzalloc(sizeof(*p));
    if (!p)
        return;
    p->parent = d;
    p->start = start;
    size_t len = strlen(d->name);
    // nvme0n1 -> nvme0n1p1, sda -> sda1
    bool digit = len && d->name[len - 1] >= '0' && d->name[len - 1] <= '9';
    snprintf(p->blk.name, sizeof p->blk.name, "%s%s%d", d->name, digit ? "p" : "", n);
    p->blk.sectors = count;
    p->blk.read = part_read;
    p->blk.write = part_write;
    p->blk.priv = p;
    p->blk.is_part = true;
    strlcpy(p->blk.model, kind, sizeof p->blk.model);
    blk_register(&p->blk);
}

static const uint8_t esp_guid[16] = { 0x28, 0x73, 0x2a, 0xc1, 0x1f, 0xf8, 0xd2, 0x11,
                                      0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b };

static void scan_gpt(struct blkdev *d)
{
    uint8_t *hdr = kmalloc(SECTOR_SIZE);
    if (!hdr)
        return;
    if (d->read(d, 1, 1, hdr) != 0 || memcmp(hdr, "EFI PART", 8) != 0) {
        kfree(hdr);
        return;
    }
    uint64_t entries = *(uint64_t *)(hdr + 72);
    uint32_t count = *(uint32_t *)(hdr + 80), esize = *(uint32_t *)(hdr + 84);
    kfree(hdr);
    if (esize < 128 || esize > 512 || count > 256)
        return;
    size_t bytes = (size_t)count * esize;
    uint32_t secs = (uint32_t)((bytes + SECTOR_SIZE - 1) / SECTOR_SIZE);
    uint8_t *buf = kmalloc((size_t)secs * SECTOR_SIZE);
    if (!buf || d->read(d, entries, secs, buf) != 0) {
        kfree(buf);
        return;
    }
    static const uint8_t zero[16];
    int n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = buf + (size_t)i * esize;
        if (memcmp(e, zero, 16) == 0)
            continue;
        uint64_t first = *(const uint64_t *)(e + 32), last = *(const uint64_t *)(e + 40);
        n++;
        if (last >= first)
            add_part(d, n, first, last - first + 1,
                     memcmp(e, esp_guid, 16) == 0 ? "efi system" : "gpt");
    }
    kfree(buf);
}

void blk_scan_partitions(struct blkdev *d)
{
    uint8_t *mbr = kmalloc(SECTOR_SIZE);
    if (!mbr)
        return;
    if (d->read(d, 0, 1, mbr) != 0 || mbr[510] != 0x55 || mbr[511] != 0xAA) {
        kfree(mbr);
        return;
    }
    // a filesystem that starts right at sector 0 has no partition table
    if (memcmp(mbr + 82, "FAT32", 5) == 0 || memcmp(mbr + 54, "FAT", 3) == 0) {
        kfree(mbr);
        return;
    }
    bool gpt = false;
    for (int i = 0; i < 4; i++)
        if (mbr[446 + i * 16 + 4] == 0xEE)
            gpt = true;
    if (gpt) {
        kfree(mbr);
        scan_gpt(d);
        return;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *e = mbr + 446 + i * 16;
        uint32_t start = (uint32_t)(e[8] | e[9] << 8 | e[10] << 16 | (uint32_t)e[11] << 24);
        uint32_t count = (uint32_t)(e[12] | e[13] << 8 | e[14] << 16 | (uint32_t)e[15] << 24);
        if (!e[4] || !start || (e[0] != 0 && e[0] != 0x80))
            continue;
        add_part(d, i + 1, start, count, e[4] == 0xEF ? "efi system" : "mbr");
    }
    kfree(mbr);
}

struct busy_q {
    const char *prefix;
    bool busy;
};

static void check_busy(struct mount *m, void *ctx)
{
    struct busy_q *q = ctx;
    if (strncmp(m->source, q->prefix, strlen(q->prefix)) == 0)
        q->busy = true;
}

// forgets d's partitions and reads the table again, for after repartitioning
int blk_rescan(struct blkdev *d)
{
    char prefix[32];
    snprintf(prefix, sizeof prefix, "/dev/%s", d->name);
    struct busy_q q = { prefix, false };
    vfs_each_mount(check_busy, &q);
    if (q.busy)
        return ZK_EBUSY;
    blk_sync();
    for (int i = 0; i < ndevs; ) {
        struct blkdev *p = devs[i];
        if (!p->is_part || ((struct part *)p->priv)->parent != d) {
            i++;
            continue;
        }
        char path[32];
        snprintf(path, sizeof path, "/dev/%s", p->name);
        vfs_unlink(path);
        blk_invalidate(p);
        devs[i] = devs[--ndevs];
        // the vnode may still be referenced by an open file, so the struct
        // is left alone rather than freed
    }
    blk_invalidate(d);
    blk_scan_partitions(d);
    return 0;
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
