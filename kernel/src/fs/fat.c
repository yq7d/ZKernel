// fat32 with long names, read and write. metadata goes straight to the disk
// (the block cache is write-through), so a crash loses at most the op in flight
#include "fs/vfs.h"
#include "drivers/blk.h"
#include "drivers/rtc.h"
#include "kprintf.h"
#include "lib/string.h"
#include "lib/time.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define ATTR_RO     0x01
#define ATTR_HIDDEN 0x02
#define ATTR_SYSTEM 0x04
#define ATTR_VOLUME 0x08
#define ATTR_DIR    0x10
#define ATTR_ARCH   0x20
#define ATTR_LFN    0x0F

#define NT_LOWER_BASE 0x08
#define NT_LOWER_EXT  0x10

#define FAT_EOC   0x0FFFFFF8
#define FAT_BAD   0x0FFFFFF7
#define FAT_MASK  0x0FFFFFFF

#define MAX_LFN_SLOTS 20
#define MAX_NAME      255

struct fat_sb {
    struct blkdev *dev;
    uint64_t part_off;
    uint32_t bps, spc, cluster_size;
    uint32_t nfats, fat_sectors, root_clus, nclusters, fsinfo_sector;
    uint64_t fat_off, data_off;
    uint32_t free_count, next_free;
    struct mutex lock;
    struct fat_node *nodes;
    struct fat_node *root;
    bool unmounted;
    bool fsinfo_dirty;
    uint32_t uid, gid, fmode, dmode;    // fat has no owners, the mount picks them
};

struct fat_node {
    struct vnode v;
    struct fat_sb *sb;
    uint32_t first;       // first cluster, 0 for an empty file
    uint64_t ent_off;     // short dir entry on disk, 0 for the root
    bool unlinked;        // clusters get freed when the last reference goes
    uint32_t cur_idx, cur_clus;    // last cluster walked to, for sequential io
    struct fat_node *next;
    bool detached;        // dropped from the cache while its release was pending
};

// one directory entry as found by a scan
struct dent {
    char name[MAX_NAME + 1];
    uint8_t raw[32];               // the short entry
    uint64_t offs[MAX_LFN_SLOTS + 1];    // every slot it uses, short entry last
    int nslots;
};

static const struct vnode_ops ops;
static int write_fsinfo(struct fat_sb *sb);

// ends a locked op, keeping fsinfo's free count in step with the fat
static void unlock(struct fat_sb *sb)
{
    if (sb->fsinfo_dirty) {
        write_fsinfo(sb);
        sb->fsinfo_dirty = false;
    }
    mutex_unlock(&sb->lock);
}

#define NODE(vn) ((struct fat_node *)(vn))

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static void wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, (uint16_t)v); wr16(p + 2, (uint16_t)(v >> 16)); }

static int disk_rd(struct fat_sb *sb, uint64_t off, void *buf, size_t len)
{
    return blk_read(sb->dev, sb->part_off + off, buf, len);
}

static int disk_wr(struct fat_sb *sb, uint64_t off, const void *buf, size_t len)
{
    return blk_write(sb->dev, sb->part_off + off, buf, len);
}

static bool valid_clus(struct fat_sb *sb, uint32_t c)
{
    return c >= 2 && c < sb->nclusters + 2;
}

static uint64_t clus_off(struct fat_sb *sb, uint32_t c)
{
    return sb->data_off + (uint64_t)(c - 2) * sb->cluster_size;
}

static int fat_get(struct fat_sb *sb, uint32_t c, uint32_t *out)
{
    uint8_t b[4];
    int err = disk_rd(sb, sb->fat_off + (uint64_t)c * 4, b, 4);
    if (!err)
        *out = rd32(b) & FAT_MASK;
    return err;
}

static int fat_set(struct fat_sb *sb, uint32_t c, uint32_t val)
{
    for (uint32_t i = 0; i < sb->nfats; i++) {
        uint64_t off = sb->fat_off + (uint64_t)i * sb->fat_sectors * sb->bps + (uint64_t)c * 4;
        uint8_t b[4];
        int err = disk_rd(sb, off, b, 4);
        if (err)
            return err;
        wr32(b, (rd32(b) & ~FAT_MASK) | (val & FAT_MASK));
        err = disk_wr(sb, off, b, 4);
        if (err)
            return err;
    }
    return 0;
}

static int zero_cluster(struct fat_sb *sb, uint32_t c)
{
    uint8_t *z = kzalloc(sb->cluster_size);
    if (!z)
        return ZK_ENOMEM;
    int err = disk_wr(sb, clus_off(sb, c), z, sb->cluster_size);
    kfree(z);
    return err;
}

// a zeroed cluster, linked after prev unless prev is 0
static int alloc_cluster(struct fat_sb *sb, uint32_t prev, uint32_t *out)
{
    if (sb->free_count == 0)
        return ZK_ENOSPC;
    uint32_t c = valid_clus(sb, sb->next_free) ? sb->next_free : 2;
    for (uint32_t n = 0; n < sb->nclusters; n++) {
        uint32_t val;
        int err = fat_get(sb, c, &val);
        if (err)
            return err;
        if (val == 0) {
            err = zero_cluster(sb, c);
            if (!err)
                err = fat_set(sb, c, FAT_MASK);
            if (!err && prev)
                err = fat_set(sb, prev, c);
            if (err)
                return err;
            sb->free_count--;
            sb->next_free = c + 1;
            sb->fsinfo_dirty = true;
            *out = c;
            return 0;
        }
        c = c + 1 < sb->nclusters + 2 ? c + 1 : 2;
    }
    sb->free_count = 0;
    return ZK_ENOSPC;
}

static int free_chain(struct fat_sb *sb, uint32_t c)
{
    for (uint32_t n = 0; valid_clus(sb, c) && n < sb->nclusters; n++) {
        uint32_t next;
        int err = fat_get(sb, c, &next);
        if (!err)
            err = fat_set(sb, c, 0);
        if (err)
            return err;
        sb->free_count++;
        sb->fsinfo_dirty = true;
        if (c < sb->next_free)
            sb->next_free = c;
        c = next;
    }
    return 0;
}

// cluster number idx of the chain, growing it if extend is set
static int chain_nth(struct fat_node *n, uint32_t idx, bool extend, uint32_t *out)
{
    struct fat_sb *sb = n->sb;
    if (!n->first) {
        if (!extend)
            return ZK_EIO;
        int err = alloc_cluster(sb, 0, &n->first);
        if (err)
            return err;
        n->cur_idx = 0;
        n->cur_clus = n->first;
    }
    uint32_t i = 0, c = n->first;
    if (n->cur_clus && n->cur_idx <= idx) {
        i = n->cur_idx;
        c = n->cur_clus;
    }
    while (i < idx) {
        uint32_t next;
        int err = fat_get(sb, c, &next);
        if (err)
            return err;
        if (next >= FAT_EOC) {
            if (!extend)
                return ZK_EIO;
            err = alloc_cluster(sb, c, &next);
            if (err)
                return err;
        } else if (!valid_clus(sb, next)) {
            return ZK_EIO;
        }
        c = next;
        i++;
    }
    n->cur_idx = idx;
    n->cur_clus = c;
    *out = c;
    return 0;
}

static void fat_datetime(int64_t t, uint16_t *date, uint16_t *time)
{
    struct datetime d;
    epoch_to_datetime(t, &d);
    if (d.year < 1980)
        d = (struct datetime){ .year = 1980, .month = 1, .day = 1 };
    *date = (uint16_t)(((d.year - 1980) << 9) | (d.month << 5) | d.day);
    *time = (uint16_t)((d.hour << 11) | (d.min << 5) | (d.sec / 2));
}

static int64_t fat_epoch(uint16_t date, uint16_t time)
{
    struct datetime d = {
        .year = 1980 + (date >> 9), .month = (date >> 5) & 15, .day = date & 31,
        .hour = time >> 11, .min = (time >> 5) & 63, .sec = (time & 31) * 2,
    };
    if (d.month < 1 || d.day < 1)
        return 0;
    return datetime_to_epoch(&d);
}

static uint32_t ent_cluster(const uint8_t *e)
{
    return (uint32_t)rd16(e + 20) << 16 | rd16(e + 26);
}

static uint8_t sfn_checksum(const uint8_t *name)
{
    uint8_t sum = 0;
    for (int i = 0; i < 11; i++)
        sum = (uint8_t)(((sum & 1) << 7) + (sum >> 1) + name[i]);
    return sum;
}

static void short_to_name(const uint8_t *e, char *out)
{
    size_t n = 0;
    uint8_t nt = e[12];
    for (int i = 0; i < 8 && e[i] != ' '; i++) {
        char c = (char)(i == 0 && e[0] == 0x05 ? 0xE5 : e[i]);
        out[n++] = (nt & NT_LOWER_BASE) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
    }
    if (e[8] != ' ') {
        out[n++] = '.';
        for (int i = 8; i < 11 && e[i] != ' '; i++) {
            char c = (char)e[i];
            out[n++] = (nt & NT_LOWER_EXT) && c >= 'A' && c <= 'Z' ? (char)(c + 32) : c;
        }
    }
    out[n] = '\0';
}

static bool name_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'a' && *a <= 'z' ? (char)(*a - 32) : *a;
        char y = *b >= 'a' && *b <= 'z' ? (char)(*b - 32) : *b;
        if (x != y)
            return false;
    }
    return *a == *b;
}

// calls fn for every live entry in the dir (including . and ..). fn returns
// true to stop. with free_run set, it instead looks for that many free slots
// in a row and reports them in d->offs
typedef bool (*scan_fn)(struct dent *d, void *ctx);

static int scan(struct fat_sb *sb, uint32_t dir, scan_fn fn, void *ctx,
                int free_run, struct dent *found)
{
    uint8_t *buf = kmalloc(sb->cluster_size);
    struct dent *d = kmalloc(sizeof(*d));
    if (!buf || !d) {
        kfree(buf);
        kfree(d);
        return ZK_ENOMEM;
    }

    int err = 0, lfn_total = 0, lfn_seen = 0, nfree = 0;
    uint8_t lfn_sum = 0;
    bool stop = false, hit = false;
    uint32_t c = dir;
    for (uint32_t hops = 0; valid_clus(sb, c) && !stop && hops < sb->nclusters; hops++) {
        err = disk_rd(sb, clus_off(sb, c), buf, sb->cluster_size);
        if (err)
            break;
        for (uint32_t i = 0; i < sb->cluster_size / 32 && !stop; i++) {
            uint8_t *e = buf + i * 32;
            uint64_t off = clus_off(sb, c) + i * 32;

            if (free_run) {
                if (e[0] == 0 || e[0] == 0xE5) {
                    found->offs[nfree++] = off;
                    if (nfree == free_run) {
                        found->nslots = nfree;
                        stop = hit = true;
                    }
                } else {
                    nfree = 0;
                }
                continue;
            }

            if (e[0] == 0) {
                stop = true;
                break;
            }
            if (e[0] == 0xE5) {
                lfn_total = 0;
                continue;
            }
            if (e[11] == ATTR_LFN) {
                int ord = e[0] & 0x1F;
                if (e[0] & 0x40) {
                    lfn_total = ord;
                    lfn_seen = 0;
                    lfn_sum = e[13];
                    memset(d->name, 0, sizeof d->name);
                }
                if (!lfn_total || ord < 1 || ord > MAX_LFN_SLOTS || e[13] != lfn_sum ||
                    ord != lfn_total - lfn_seen) {
                    lfn_total = 0;
                    continue;
                }
                static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
                for (int k = 0; k < 13; k++) {
                    uint16_t ch = rd16(e + pos[k]);
                    int at = (ord - 1) * 13 + k;
                    if (ch == 0 || ch == 0xFFFF || at >= MAX_NAME)
                        break;
                    d->name[at] = ch < 0x80 ? (char)ch : '?';
                }
                d->offs[lfn_seen++] = off;
                continue;
            }
            if (e[11] & ATTR_VOLUME) {
                lfn_total = 0;
                continue;
            }

            bool have_lfn = lfn_total && lfn_seen == lfn_total && sfn_checksum(e) == lfn_sum;
            if (!have_lfn) {
                short_to_name(e, d->name);
                lfn_seen = 0;
            }
            d->offs[lfn_seen] = off;
            d->nslots = lfn_seen + 1;
            memcpy(d->raw, e, 32);
            lfn_total = 0;
            if (fn(d, ctx)) {
                if (found)
                    memcpy(found, d, sizeof(*d));
                stop = hit = true;
            }
        }
        if (stop)
            break;
        uint32_t next;
        err = fat_get(sb, c, &next);
        if (err)
            break;
        c = next;
    }
    kfree(buf);
    kfree(d);
    if (err)
        return err;
    if (free_run && !hit)
        return ZK_ENOSPC;
    return hit ? 1 : 0;
}

static bool match_name(struct dent *d, void *ctx)
{
    return name_eq(d->name, ctx);
}

// finds name in dir. returns 1 found, 0 not found, or an error
static int find(struct fat_sb *sb, uint32_t dir, const char *name, struct dent *out)
{
    return scan(sb, dir, match_name, (void *)name, 0, out);
}

static uint32_t dir_cluster(struct fat_node *n)
{
    return n == n->sb->root ? n->sb->root_clus : n->first;
}

// what a .. entry stores for this dir, the root is 0
static uint32_t dotdot_cluster(struct fat_node *n)
{
    return n == n->sb->root ? 0 : n->first;
}

// lock held. the active node for an entry, or a new one
static struct fat_node *get_node(struct fat_sb *sb, const struct dent *d)
{
    uint64_t off = d->offs[d->nslots - 1];
    for (struct fat_node **pp = &sb->nodes; *pp; pp = &(*pp)->next) {
        struct fat_node *n = *pp;
        if (n->ent_off != off)
            continue;
        if (vnode_get_unless_zero(&n->v))
            return n;
        // its release is coming and frees it, a fresh node takes its place
        *pp = n->next;
        n->detached = true;
        break;
    }
    struct fat_node *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    const uint8_t *e = d->raw;
    bool dir = e[11] & ATTR_DIR;
    n->v = (struct vnode){ .type = dir ? VNODE_DIR : VNODE_FILE, .refs = 1,
                           .ino = vfs_next_ino(), .size = dir ? 0 : rd32(e + 28),
                           .mtime = fat_epoch(rd16(e + 24), rd16(e + 22)), .ops = &ops,
                           .uid = sb->uid, .gid = sb->gid,
                           .mode = dir ? sb->dmode : sb->fmode };
    n->sb = sb;
    n->first = ent_cluster(e);
    n->ent_off = off;
    n->next = sb->nodes;
    sb->nodes = n;
    return n;
}

// writes first cluster, size and mtime back into the dir entry
static int update_entry(struct fat_node *n)
{
    if (!n->ent_off)
        return 0;
    uint8_t e[32];
    int err = disk_rd(n->sb, n->ent_off, e, 32);
    if (err)
        return err;
    uint16_t date, time;
    n->v.mtime = rtc_now();
    fat_datetime(n->v.mtime, &date, &time);
    wr16(e + 20, (uint16_t)(n->first >> 16));
    wr16(e + 26, (uint16_t)n->first);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 18, date);
    if (n->v.type == VNODE_FILE)
        wr32(e + 28, (uint32_t)n->v.size);
    return disk_wr(n->sb, n->ent_off, e, 32);
}

static int64_t f_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct fat_node *n = NODE(v);
    struct fat_sb *sb = n->sb;
    mutex_lock(&sb->lock);
    if (off >= v->size) {
        unlock(sb);
        return 0;
    }
    if (len > v->size - off)
        len = v->size - off;

    uint8_t *out = buf;
    size_t done = 0;
    int err = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t c;
        err = chain_nth(n, (uint32_t)(pos / sb->cluster_size), false, &c);
        if (err)
            break;
        size_t in = pos % sb->cluster_size;
        size_t chunk = sb->cluster_size - in < len - done ? sb->cluster_size - in : len - done;
        err = disk_rd(sb, clus_off(sb, c) + in, out + done, chunk);
        if (err)
            break;
        done += chunk;
    }
    unlock(sb);
    return done ? (int64_t)done : err;
}

// lock held. zero fills from the current size up to size
static int grow_to(struct fat_node *n, uint64_t size)
{
    struct fat_sb *sb = n->sb;
    uint64_t cur = n->v.size;
    if (size <= cur)
        return 0;
    // clusters come zeroed, so only the tail of the last used one needs clearing
    if (cur % sb->cluster_size && n->first) {
        uint32_t c;
        int err = chain_nth(n, (uint32_t)(cur / sb->cluster_size), false, &c);
        if (err)
            return err;
        size_t in = cur % sb->cluster_size;
        size_t z = sb->cluster_size - in;
        if (z > size - cur)
            z = size - cur;
        uint8_t *zeros = kzalloc(z);
        if (!zeros)
            return ZK_ENOMEM;
        err = disk_wr(sb, clus_off(sb, c) + in, zeros, z);
        kfree(zeros);
        if (err)
            return err;
    }
    uint32_t c;
    return chain_nth(n, (uint32_t)((size - 1) / sb->cluster_size), true, &c);
}

static int64_t f_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    struct fat_node *n = NODE(v);
    struct fat_sb *sb = n->sb;
    if (len == 0)
        return 0;
    if (off + len > 0xFFFFFFFFUL)
        return ZK_EFBIG;

    mutex_lock(&sb->lock);
    int err = 0;
    if (off > v->size) {
        err = grow_to(n, off);
        if (!err)
            v->size = off;
    }
    const uint8_t *in = buf;
    size_t done = 0;
    while (!err && done < len) {
        uint64_t pos = off + done;
        uint32_t c;
        err = chain_nth(n, (uint32_t)(pos / sb->cluster_size), true, &c);
        if (err)
            break;
        size_t o = pos % sb->cluster_size;
        size_t chunk = sb->cluster_size - o < len - done ? sb->cluster_size - o : len - done;
        err = disk_wr(sb, clus_off(sb, c) + o, in + done, chunk);
        if (err)
            break;
        done += chunk;
    }
    if (off + done > v->size)
        v->size = off + done;
    int uerr = update_entry(n);
    unlock(sb);
    if (done)
        return (int64_t)done;
    return err ? err : uerr;
}

static int f_truncate(struct vnode *v, uint64_t size)
{
    struct fat_node *n = NODE(v);
    struct fat_sb *sb = n->sb;
    if (size > 0xFFFFFFFFUL)
        return ZK_EFBIG;
    mutex_lock(&sb->lock);
    int err = 0;
    if (size > v->size) {
        err = grow_to(n, size);
    } else if (size == 0) {
        err = free_chain(sb, n->first);
        n->first = 0;
    } else if (n->first) {
        uint32_t keep = (uint32_t)((size - 1) / sb->cluster_size), last, rest;
        err = chain_nth(n, keep, false, &last);
        if (!err)
            err = fat_get(sb, last, &rest);
        if (!err && rest < FAT_EOC) {
            err = fat_set(sb, last, FAT_MASK);
            if (!err)
                err = free_chain(sb, rest);
        }
    }
    n->cur_clus = n->cur_idx = 0;
    if (!err)
        v->size = size;
    int uerr = update_entry(n);
    unlock(sb);
    return err ? err : uerr;
}

static int d_lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    struct fat_node *dn = NODE(dir);
    struct fat_sb *sb = dn->sb;
    struct dent *d = kmalloc(sizeof(*d));
    if (!d)
        return ZK_ENOMEM;
    mutex_lock(&sb->lock);
    int r = find(sb, dir_cluster(dn), name, d);
    struct fat_node *n = NULL;
    if (r == 1) {
        // .. of a top level dir points at cluster 0, which means the root
        if (name_eq(name, "..") && ent_cluster(d->raw) == 0) {
            n = sb->root;
            vnode_get(&n->v);
        } else if (name_eq(name, ".")) {
            n = dn;
            vnode_get(&n->v);
        } else {
            n = get_node(sb, d);
        }
    }
    unlock(sb);
    kfree(d);
    if (r < 0)
        return r;
    if (r == 0)
        return ZK_ENOENT;
    if (!n)
        return ZK_ENOMEM;
    *out = &n->v;
    return 0;
}

static bool valid_sfn_char(char c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
           (c && strchr("!#$%&'()-@^_`{}~", c));
}

// an exact 8.3 name needs no long entries. returns false if it isn't one
static bool exact_sfn(const char *name, uint8_t *sfn, uint8_t *nt)
{
    const char *dot = strrchr(name, '.');
    size_t blen = dot ? (size_t)(dot - name) : strlen(name);
    size_t elen = dot ? strlen(dot + 1) : 0;
    if (blen == 0 || blen > 8 || elen > 3 || (dot && elen == 0))
        return false;
    memset(sfn, ' ', 11);
    *nt = 0;
    bool lo = false, up = false;
    for (size_t i = 0; i < blen; i++) {
        char c = name[i];
        if (!valid_sfn_char(c))
            return false;
        lo |= c >= 'a' && c <= 'z';
        up |= c >= 'A' && c <= 'Z';
        sfn[i] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    if (lo && up)
        return false;
    if (lo)
        *nt |= NT_LOWER_BASE;
    lo = up = false;
    for (size_t i = 0; i < elen; i++) {
        char c = dot[1 + i];
        if (!valid_sfn_char(c))
            return false;
        lo |= c >= 'a' && c <= 'z';
        up |= c >= 'A' && c <= 'Z';
        sfn[8 + i] = (uint8_t)(c >= 'a' && c <= 'z' ? c - 32 : c);
    }
    if (lo && up)
        return false;
    if (lo)
        *nt |= NT_LOWER_EXT;
    if (sfn[0] == 0xE5)
        sfn[0] = 0x05;
    return true;
}

static bool match_sfn(struct dent *d, void *ctx)
{
    return memcmp(d->raw, ctx, 11) == 0;
}

// BASIS~N.EXT that nothing in the dir uses yet
static int make_alias(struct fat_sb *sb, uint32_t dir, const char *name, uint8_t *sfn)
{
    const char *dot = strrchr(name, '.');
    if (dot == name)
        dot = NULL;
    char base[8];
    size_t bl = 0;
    for (const char *p = name; *p && p != dot && bl < 6; p++) {
        if (*p == ' ' || *p == '.')
            continue;
        char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
        base[bl++] = valid_sfn_char(c) ? c : '_';
    }
    if (bl == 0)
        base[bl++] = '_';

    for (int num = 1; num < 1000000; num++) {
        char tail[8];
        int tl = 0, t = num;
        char digits[8];
        int nd = 0;
        do {
            digits[nd++] = (char)('0' + t % 10);
            t /= 10;
        } while (t);
        tail[tl++] = '~';
        while (nd)
            tail[tl++] = digits[--nd];

        memset(sfn, ' ', 11);
        size_t keep = bl + (size_t)tl > 8 ? 8 - (size_t)tl : bl;
        memcpy(sfn, base, keep);
        memcpy(sfn + keep, tail, (size_t)tl);
        if (dot) {
            size_t el = 0;
            for (const char *p = dot + 1; *p && el < 3; p++) {
                char c = *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p;
                if (c != ' ')
                    sfn[8 + el++] = (uint8_t)(valid_sfn_char(c) ? c : '_');
            }
        }
        int r = scan(sb, dir, match_sfn, sfn, 0, NULL);
        if (r < 0)
            return r;
        if (r == 0)
            return 0;
    }
    return ZK_EEXIST;
}

// lock held. free slots for count entries, growing the dir if needed
static int find_slots(struct fat_node *dn, int count, struct dent *slots)
{
    struct fat_sb *sb = dn->sb;
    for (;;) {
        int r = scan(sb, dir_cluster(dn), NULL, NULL, count, slots);
        if (r != ZK_ENOSPC)
            return r < 0 ? r : 0;
        // append a cluster to the dir
        uint32_t last = dir_cluster(dn), next, c;
        for (uint32_t hops = 0; hops < sb->nclusters; hops++) {
            int err = fat_get(sb, last, &next);
            if (err)
                return err;
            if (next >= FAT_EOC)
                break;
            last = next;
        }
        int err = alloc_cluster(sb, last, &c);
        if (err)
            return err;
    }
}

// lock held. writes the long entries plus the short entry e for name
static int write_entries(struct fat_node *dn, const char *name, uint8_t *e, bool need_lfn,
                         uint64_t *short_off)
{
    struct fat_sb *sb = dn->sb;
    size_t len = strlen(name);
    int nlfn = need_lfn ? (int)((len + 12) / 13) : 0;
    struct dent *slots = kmalloc(sizeof(*slots));
    if (!slots)
        return ZK_ENOMEM;
    int err = find_slots(dn, nlfn + 1, slots);
    if (err) {
        kfree(slots);
        return err;
    }

    uint8_t sum = sfn_checksum(e);
    static const int pos[13] = { 1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30 };
    for (int k = 0; k < nlfn && !err; k++) {
        int ord = nlfn - k;
        uint8_t l[32];
        memset(l, 0, sizeof l);
        l[0] = (uint8_t)(ord | (k == 0 ? 0x40 : 0));
        l[11] = ATTR_LFN;
        l[13] = sum;
        for (int i = 0; i < 13; i++) {
            size_t at = (size_t)(ord - 1) * 13 + (size_t)i;
            uint16_t ch = at < len ? (uint8_t)name[at] : at == len ? 0 : 0xFFFF;
            wr16(l + pos[i], ch);
        }
        err = disk_wr(sb, slots->offs[k], l, 32);
    }
    if (!err)
        err = disk_wr(sb, slots->offs[nlfn], e, 32);
    if (!err)
        *short_off = slots->offs[nlfn];
    kfree(slots);
    return err;
}

static int check_name(const char *name)
{
    if (strlen(name) > MAX_NAME)
        return ZK_ENAMETOOLONG;
    for (const char *p = name; *p; p++)
        if ((unsigned char)*p < 0x20 || strchr("\"*/:<>?\\|", *p))
            return ZK_EINVAL;
    return 0;
}

// lock held. builds the short entry and its name for a new file
static int new_entry(struct fat_node *dn, const char *name, uint8_t attr, uint32_t first,
                     uint8_t *e, bool *need_lfn)
{
    memset(e, 0, 32);
    uint8_t nt = 0;
    *need_lfn = !exact_sfn(name, e, &nt);
    if (*need_lfn) {
        int err = make_alias(dn->sb, dir_cluster(dn), name, e);
        if (err)
            return err;
        nt = 0;
    }
    e[11] = attr;
    e[12] = nt;
    uint16_t date, time;
    fat_datetime(rtc_now(), &date, &time);
    wr16(e + 14, time);
    wr16(e + 16, date);
    wr16(e + 18, date);
    wr16(e + 22, time);
    wr16(e + 24, date);
    wr16(e + 20, (uint16_t)(first >> 16));
    wr16(e + 26, (uint16_t)first);
    return 0;
}

static int d_create(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out)
{
    struct fat_node *dn = NODE(dir);
    struct fat_sb *sb = dn->sb;
    if (type != VNODE_FILE && type != VNODE_DIR)
        return ZK_EPERM;
    int err = check_name(name);
    if (err)
        return err;
    struct dent *d = kmalloc(sizeof(*d));
    if (!d)
        return ZK_ENOMEM;

    mutex_lock(&sb->lock);
    if (dn->unlinked) {
        err = ZK_ENOENT;
        goto out;
    }
    int r = find(sb, dir_cluster(dn), name, d);
    if (r != 0) {
        err = r < 0 ? r : ZK_EEXIST;
        goto out;
    }

    uint32_t first = 0;
    if (type == VNODE_DIR) {
        err = alloc_cluster(sb, 0, &first);
        if (err)
            goto out;
        // . and ..
        uint8_t dots[64];
        bool unused;
        new_entry(dn, "x", ATTR_DIR, first, dots, &unused);
        memset(dots, ' ', 11);
        dots[0] = '.';
        new_entry(dn, "x", ATTR_DIR, dotdot_cluster(dn), dots + 32, &unused);
        memset(dots + 32, ' ', 11);
        dots[32] = dots[33] = '.';
        dots[12] = dots[44] = 0;
        err = disk_wr(sb, clus_off(sb, first), dots, 64);
        if (err) {
            free_chain(sb, first);
            goto out;
        }
    }

    uint8_t e[32];
    bool need_lfn;
    err = new_entry(dn, name, type == VNODE_DIR ? ATTR_DIR : ATTR_ARCH, first, e, &need_lfn);
    uint64_t off = 0;
    if (!err)
        err = write_entries(dn, name, e, need_lfn, &off);
    if (err) {
        if (first)
            free_chain(sb, first);
        goto out;
    }
    update_entry(dn);    // dir mtime

    memcpy(d->raw, e, 32);
    d->offs[0] = off;
    d->nslots = 1;
    struct fat_node *n = get_node(sb, d);
    if (!n)
        err = ZK_ENOMEM;
    else
        *out = &n->v;
out:
    unlock(sb);
    kfree(d);
    return err;
}

static bool not_dot(struct dent *d, void *ctx)
{
    (void)ctx;
    return strcmp(d->name, ".") != 0 && strcmp(d->name, "..") != 0;
}

// lock held. marks the entry's slots deleted
static int erase_entry(struct fat_sb *sb, const struct dent *d)
{
    uint8_t del = 0xE5;
    for (int i = 0; i < d->nslots; i++) {
        int err = disk_wr(sb, d->offs[i], &del, 1);
        if (err)
            return err;
    }
    return 0;
}

// lock held. node for an entry if someone has it open
static struct fat_node *active(struct fat_sb *sb, uint64_t ent_off)
{
    for (struct fat_node *n = sb->nodes; n; n = n->next)
        if (n->ent_off == ent_off)
            return n;
    return NULL;
}

// lock held. removes d, freeing its clusters now or when it's closed
static int remove_entry(struct fat_sb *sb, const struct dent *d)
{
    uint32_t first = ent_cluster(d->raw);
    if (d->raw[11] & ATTR_DIR) {
        int r = scan(sb, first, not_dot, NULL, 0, NULL);
        if (r != 0)
            return r < 0 ? r : ZK_ENOTEMPTY;
    }
    int err = erase_entry(sb, d);
    if (err)
        return err;
    struct fat_node *n = active(sb, d->offs[d->nslots - 1]);
    if (n) {
        n->unlinked = true;
        n->ent_off = 0;    // nothing on disk points at it any more
        return 0;
    }
    return free_chain(sb, first);
}

static int d_unlink(struct vnode *dir, const char *name)
{
    struct fat_node *dn = NODE(dir);
    struct fat_sb *sb = dn->sb;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return ZK_EINVAL;
    struct dent *d = kmalloc(sizeof(*d));
    if (!d)
        return ZK_ENOMEM;
    mutex_lock(&sb->lock);
    int r = find(sb, dir_cluster(dn), name, d);
    int err = r < 0 ? r : r == 0 ? ZK_ENOENT : remove_entry(sb, d);
    if (!err)
        update_entry(dn);
    unlock(sb);
    kfree(d);
    return err;
}

struct nth_ctx {
    size_t want, seen;
};

static bool nth(struct dent *d, void *ctx)
{
    struct nth_ctx *c = ctx;
    if (strcmp(d->name, ".") == 0 || strcmp(d->name, "..") == 0)
        return false;
    return c->seen++ == c->want;
}

static int d_readdir(struct vnode *dir, size_t index, struct zk_dirent *out)
{
    struct fat_node *dn = NODE(dir);
    struct fat_sb *sb = dn->sb;
    struct dent *d = kmalloc(sizeof(*d));
    if (!d)
        return ZK_ENOMEM;
    struct nth_ctx c = { index, 0 };
    mutex_lock(&sb->lock);
    int r = scan(sb, dir_cluster(dn), nth, &c, 0, d);
    unlock(sb);
    if (r == 1) {
        out->type = d->raw[11] & ATTR_DIR ? ZK_TYPE_DIR : ZK_TYPE_FILE;
        strlcpy(out->name, d->name, sizeof out->name);
    }
    kfree(d);
    return r;
}

static int d_rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    struct fat_node *od = NODE(odir), *nd = NODE(ndir);
    struct fat_sb *sb = od->sb;
    if (nd->sb != sb)
        return ZK_EXDEV;
    int err = check_name(nname);
    if (err)
        return err;
    struct dent *src = kmalloc(sizeof(*src)), *dst = kmalloc(sizeof(*dst));
    if (!src || !dst) {
        kfree(src);
        kfree(dst);
        return ZK_ENOMEM;
    }

    mutex_lock(&sb->lock);
    int r = find(sb, dir_cluster(od), oname, src);
    if (r != 1) {
        err = r < 0 ? r : ZK_ENOENT;
        goto out;
    }
    bool is_dir = src->raw[11] & ATTR_DIR;
    r = find(sb, dir_cluster(nd), nname, dst);
    if (r < 0) {
        err = r;
        goto out;
    }
    if (r == 1) {
        if (dst->offs[dst->nslots - 1] == src->offs[src->nslots - 1])
            goto out;    // same file under another case, nothing to do
        bool dst_dir = dst->raw[11] & ATTR_DIR;
        if (is_dir && !dst_dir)
            err = ZK_ENOTDIR;
        else if (!is_dir && dst_dir)
            err = ZK_EISDIR;
        if (!err)
            err = remove_entry(sb, dst);
        if (err)
            goto out;
    }

    // new entry first, so a crash leaves two names rather than none
    uint8_t e[32];
    bool need_lfn;
    memcpy(e, src->raw, 32);
    uint8_t fresh[32];
    err = new_entry(nd, nname, src->raw[11], ent_cluster(src->raw), fresh, &need_lfn);
    if (err)
        goto out;
    memcpy(e, fresh, 11);
    e[12] = fresh[12];
    uint64_t off;
    err = write_entries(nd, nname, e, need_lfn, &off);
    if (err)
        goto out;
    err = erase_entry(sb, src);
    if (err)
        goto out;

    struct fat_node *n = active(sb, src->offs[src->nslots - 1]);
    if (n)
        n->ent_off = off;
    if (is_dir && od != nd) {
        // fix the moved dir's .. entry
        uint8_t dd[32];
        uint64_t dotdot = clus_off(sb, ent_cluster(e)) + 32;
        err = disk_rd(sb, dotdot, dd, 32);
        if (!err) {
            uint32_t parent = dotdot_cluster(nd);
            wr16(dd + 20, (uint16_t)(parent >> 16));
            wr16(dd + 26, (uint16_t)parent);
            err = disk_wr(sb, dotdot, dd, 32);
        }
    }
    update_entry(od);
    if (nd != od)
        update_entry(nd);
out:
    unlock(sb);
    kfree(src);
    kfree(dst);
    return err;
}

static void release(struct vnode *v)
{
    struct fat_node *n = NODE(v);
    struct fat_sb *sb = n->sb;
    mutex_lock(&sb->lock);
    for (struct fat_node **p = &sb->nodes; *p && !n->detached; p = &(*p)->next) {
        if (*p == n) {
            *p = n->next;
            break;
        }
    }
    if (n->unlinked)
        free_chain(sb, n->first);
    bool free_sb = n == sb->root && sb->unmounted;
    unlock(sb);
    kfree(n);
    if (free_sb) {
        blk_invalidate(sb->dev);
        kfree(sb);
    }
}

static const struct vnode_ops ops = {
    .read = f_read,
    .write = f_write,
    .truncate = f_truncate,
    .lookup = d_lookup,
    .create = d_create,
    .unlink = d_unlink,
    .readdir = d_readdir,
    .rename = d_rename,
    .release = release,
};

static int write_fsinfo(struct fat_sb *sb)
{
    if (!sb->fsinfo_sector || sb->fsinfo_sector == 0xFFFF)
        return 0;
    uint8_t b[8];
    uint64_t off = (uint64_t)sb->fsinfo_sector * sb->bps;
    if (disk_rd(sb, off, b, 4) != 0 || rd32(b) != 0x41615252)
        return 0;
    wr32(b, sb->free_count);
    wr32(b + 4, sb->next_free);
    return disk_wr(sb, off + 488, b, 8);
}

static int fat_statfs(struct mount *m, struct zk_statfs *st)
{
    struct fat_sb *sb = m->sb;
    mutex_lock(&sb->lock);
    st->block_size = sb->cluster_size;
    st->blocks = sb->nclusters;
    st->blocks_free = sb->free_count;
    unlock(sb);
    return 0;
}

static int fat_sync(struct mount *m)
{
    struct fat_sb *sb = m->sb;
    mutex_lock(&sb->lock);
    int err = write_fsinfo(sb);
    unlock(sb);
    return err;
}

static int fat_unmount(struct mount *m)
{
    struct fat_sb *sb = m->sb;
    mutex_lock(&sb->lock);
    // only the root may still be around
    if (sb->nodes != sb->root || sb->root->next || sb->root->v.refs > 1) {
        unlock(sb);
        return ZK_EBUSY;
    }
    write_fsinfo(sb);
    sb->unmounted = true;
    unlock(sb);
    return 0;
}

// a fat32 boot sector at off, or the first fat partition of an mbr
static int find_volume(struct blkdev *dev, uint64_t *off, uint8_t *bs)
{
    int err = blk_read(dev, 0, bs, 512);
    if (err)
        return err;
    if (bs[510] != 0x55 || bs[511] != 0xAA)
        return ZK_EINVAL;
    if (rd16(bs + 22) == 0 && rd32(bs + 36) && (bs[0] == 0xEB || bs[0] == 0xE9)) {
        *off = 0;
        return 0;
    }
    for (int i = 0; i < 4; i++) {
        const uint8_t *p = bs + 446 + i * 16;
        if (p[4] == 0x0B || p[4] == 0x0C) {
            *off = (uint64_t)rd32(p + 8) * SECTOR_SIZE;
            return blk_read(dev, *off, bs, 512);
        }
    }
    return ZK_EINVAL;
}

static int fat_mount(struct mount *m, const char *source, const char *opts)
{
    struct blkdev *dev = blk_find(source);
    if (!dev)
        return ZK_ENODEV;
    uint8_t *bs = kmalloc(512);
    struct fat_sb *sb = kzalloc(sizeof(*sb));
    struct fat_node *root = kzalloc(sizeof(*root));
    int err = ZK_ENOMEM;
    if (!bs || !sb || !root)
        goto fail;

    uint64_t off;
    err = find_volume(dev, &off, bs);
    if (err)
        goto fail;

    sb->dev = dev;
    sb->part_off = off;
    sb->bps = rd16(bs + 11);
    sb->spc = bs[13];
    uint32_t reserved = rd16(bs + 14);
    sb->nfats = bs[16];
    uint32_t total = rd16(bs + 19) ? rd16(bs + 19) : rd32(bs + 32);
    sb->fat_sectors = rd32(bs + 36);
    sb->root_clus = rd32(bs + 44);
    sb->fsinfo_sector = rd16(bs + 48);
    err = ZK_EINVAL;
    if ((sb->bps != 512 && sb->bps != 1024 && sb->bps != 2048 && sb->bps != 4096) ||
        !sb->spc || (sb->spc & (sb->spc - 1)) || !sb->nfats || rd16(bs + 17) != 0 ||
        rd16(bs + 22) != 0 || !sb->fat_sectors) {
        pr_warn("fat: %s isn't fat32\n", source);
        goto fail;
    }
    sb->cluster_size = sb->bps * sb->spc;
    sb->fat_off = (uint64_t)reserved * sb->bps;
    uint64_t data_sector = reserved + (uint64_t)sb->nfats * sb->fat_sectors;
    sb->data_off = data_sector * sb->bps;
    if (total <= data_sector)
        goto fail;
    sb->nclusters = (uint32_t)((total - data_sector) / sb->spc);
    // the fat itself may be smaller than the cluster count claims
    uint32_t fat_entries = (uint32_t)((uint64_t)sb->fat_sectors * sb->bps / 4);
    if (sb->nclusters + 2 > fat_entries)
        sb->nclusters = fat_entries - 2;
    if (sb->nclusters == 0)
        goto fail;
    if (!valid_clus(sb, sb->root_clus) || (off + data_sector * sb->bps) / SECTOR_SIZE > dev->sectors)
        goto fail;

    // count free clusters, fsinfo can't be trusted after an unclean shutdown
    uint32_t *chunk = kmalloc(4096);
    if (!chunk) {
        err = ZK_ENOMEM;
        goto fail;
    }
    sb->free_count = 0;
    sb->next_free = 0;
    for (uint32_t c = 0; c < sb->nclusters + 2; c += 1024) {
        uint32_t n = sb->nclusters + 2 - c < 1024 ? sb->nclusters + 2 - c : 1024;
        err = disk_rd(sb, sb->fat_off + (uint64_t)c * 4, chunk, n * 4);
        if (err) {
            kfree(chunk);
            goto fail;
        }
        for (uint32_t i = 0; i < n; i++) {
            if (c + i >= 2 && (chunk[i] & FAT_MASK) == 0) {
                sb->free_count++;
                if (!sb->next_free)
                    sb->next_free = c + i;
            }
        }
    }
    kfree(chunk);

    sb->uid = sb->gid = 0;
    sb->fmode = 0755;    // no exec bit on fat, so everything may run
    sb->dmode = 0755;
    mount_opt(opts, "uid", &sb->uid, 10);
    mount_opt(opts, "gid", &sb->gid, 10);
    uint32_t mode;
    if (mount_opt(opts, "mode", &mode, 8))
        sb->fmode = sb->dmode = mode & 0777;
    root->v = (struct vnode){ .type = VNODE_DIR, .refs = 1, .ino = vfs_next_ino(), .ops = &ops,
                              .uid = sb->uid, .gid = sb->gid, .mode = sb->dmode };
    root->sb = sb;
    root->first = sb->root_clus;
    sb->root = root;
    sb->nodes = root;
    m->sb = sb;
    m->root = &root->v;
    m->statfs = fat_statfs;
    m->sync = fat_sync;
    m->unmount = fat_unmount;
    kfree(bs);
    return 0;

fail:
    kfree(bs);
    kfree(sb);
    kfree(root);
    return err;
}

const struct fs_type fat_type = { "fat", fat_mount };
