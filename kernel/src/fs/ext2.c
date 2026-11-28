// ext2, read and write: permissions, owners, symlinks, sparse files.
// directories are plain linear lists (no htree), the image is made with
// -O ^dir_index. writes go straight through the block cache
#include "fs/vfs.h"
#include "drivers/blk.h"
#include "drivers/rtc.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/sync.h"

#define EXT2_MAGIC   0xEF53
#define ROOT_INO     2
#define NDIR         12
#define INCOMPAT_FILETYPE 0x0002
#define RO_COMPAT_SPARSE  0x0001
#define RO_COMPAT_LARGE   0x0002

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFCHR  0020000

#define FT_REG 1
#define FT_DIR 2
#define FT_LNK 7

#define INDEX_FL 0x1000

struct sb {
    struct blkdev *dev;
    uint64_t part;
    uint32_t bsize, ngroups, ipg, bpg, isize, first_data, inodes, blocks;
    uint32_t free_blocks, free_inodes;
    uint64_t gdt;    // byte offset of the group descriptors
    struct mutex lock;
    struct enode *nodes, *root;
    bool unmounted, readonly;
};

struct inode {
    uint16_t mode, uid;
    uint32_t size, atime, ctime, mtime, dtime;
    uint16_t gid, links;
    uint32_t blocks, flags, osd1;
    uint32_t block[15];
    uint32_t generation, file_acl, size_high, faddr;
    uint8_t osd2[12];
};

_Static_assert(sizeof(struct inode) == 128, "ext2 inodes are 128 bytes");

struct enode {
    struct vnode v;
    struct sb *sb;
    uint32_t ino;
    struct inode in;
    bool unlinked;
    struct enode *next;
};

struct dirent2 {
    uint32_t inode;
    uint16_t rec_len;
    uint8_t name_len, file_type;
    char name[];
} __attribute__((packed));

static const struct vnode_ops ops;

#define EN(vn) ((struct enode *)(vn))

static int rd(struct sb *s, uint64_t off, void *buf, size_t len)
{
    return blk_read(s->dev, s->part + off, buf, len);
}

static int wr(struct sb *s, uint64_t off, const void *buf, size_t len)
{
    return blk_write(s->dev, s->part + off, buf, len);
}

static uint64_t boff(struct sb *s, uint32_t block)
{
    return (uint64_t)block * s->bsize;
}

static uint32_t rd32(struct sb *s, uint64_t off)
{
    uint32_t v = 0;
    rd(s, off, &v, 4);
    return v;
}

// superblock and group descriptors

static void write_counts(struct sb *s)
{
    wr(s, 1024 + 12, &s->free_blocks, 4);
    wr(s, 1024 + 16, &s->free_inodes, 4);
}

struct gd {
    uint32_t block_bitmap, inode_bitmap, inode_table;
    uint16_t free_blocks, free_inodes, used_dirs, pad;
    uint32_t reserved[3];
} __attribute__((packed));

static int read_gd(struct sb *s, uint32_t g, struct gd *d)
{
    return rd(s, s->gdt + (uint64_t)g * 32, d, 32);
}

static int write_gd(struct sb *s, uint32_t g, const struct gd *d)
{
    return wr(s, s->gdt + (uint64_t)g * 32, d, 32);
}

// finds a clear bit in a bitmap block, sets it. -1 if full
static int64_t take_bit(struct sb *s, uint32_t bitmap, uint32_t limit)
{
    uint8_t *buf = kmalloc(s->bsize);
    if (!buf)
        return -1;
    int64_t found = -1;
    if (rd(s, boff(s, bitmap), buf, s->bsize) == 0) {
        for (uint32_t i = 0; i < limit && i < s->bsize * 8; i++) {
            if (!(buf[i / 8] & (1 << (i % 8)))) {
                buf[i / 8] |= (uint8_t)(1 << (i % 8));
                wr(s, boff(s, bitmap) + i / 8, &buf[i / 8], 1);
                found = i;
                break;
            }
        }
    }
    kfree(buf);
    return found;
}

static void clear_bit(struct sb *s, uint32_t bitmap, uint32_t bit)
{
    uint8_t b;
    uint64_t off = boff(s, bitmap) + bit / 8;
    rd(s, off, &b, 1);
    b &= (uint8_t)~(1 << (bit % 8));
    wr(s, off, &b, 1);
}

// zero says whether the block needs clearing, a full overwrite doesn't
static uint32_t alloc_block_z(struct sb *s, uint32_t near_group, bool zero);

static uint32_t alloc_block(struct sb *s, uint32_t near_group)
{
    return alloc_block_z(s, near_group, true);
}

static uint32_t alloc_block_z(struct sb *s, uint32_t near_group, bool zero)
{
    if (!s->free_blocks)
        return 0;
    for (uint32_t k = 0; k < s->ngroups; k++) {
        uint32_t g = (near_group + k) % s->ngroups;
        struct gd d;
        if (read_gd(s, g, &d) || !d.free_blocks)
            continue;
        uint32_t in_group = g == s->ngroups - 1 ? s->blocks - s->first_data - g * s->bpg : s->bpg;
        int64_t bit = take_bit(s, d.block_bitmap, in_group);
        if (bit < 0)
            continue;
        d.free_blocks--;
        write_gd(s, g, &d);
        s->free_blocks--;
        write_counts(s);
        uint32_t block = s->first_data + g * s->bpg + (uint32_t)bit;
        uint8_t *z = zero ? kzalloc(s->bsize) : NULL;
        if (z) {
            wr(s, boff(s, block), z, s->bsize);
            kfree(z);
        }
        return block;
    }
    return 0;
}

static void free_block(struct sb *s, uint32_t block)
{
    if (block < s->first_data || block >= s->blocks)
        return;
    uint32_t g = (block - s->first_data) / s->bpg;
    struct gd d;
    if (read_gd(s, g, &d))
        return;
    clear_bit(s, d.block_bitmap, (block - s->first_data) % s->bpg);
    d.free_blocks++;
    write_gd(s, g, &d);
    s->free_blocks++;
    write_counts(s);
}

static uint32_t alloc_inode(struct sb *s, bool dir)
{
    if (!s->free_inodes)
        return 0;
    for (uint32_t g = 0; g < s->ngroups; g++) {
        struct gd d;
        if (read_gd(s, g, &d) || !d.free_inodes)
            continue;
        int64_t bit = take_bit(s, d.inode_bitmap, s->ipg);
        if (bit < 0)
            continue;
        d.free_inodes--;
        if (dir)
            d.used_dirs++;
        write_gd(s, g, &d);
        s->free_inodes--;
        write_counts(s);
        return g * s->ipg + (uint32_t)bit + 1;
    }
    return 0;
}

static void free_inode(struct sb *s, uint32_t ino, bool dir)
{
    uint32_t g = (ino - 1) / s->ipg;
    struct gd d;
    if (read_gd(s, g, &d))
        return;
    clear_bit(s, d.inode_bitmap, (ino - 1) % s->ipg);
    d.free_inodes++;
    if (dir && d.used_dirs)
        d.used_dirs--;
    write_gd(s, g, &d);
    s->free_inodes++;
    write_counts(s);
}

static uint64_t inode_off(struct sb *s, uint32_t ino)
{
    uint32_t g = (ino - 1) / s->ipg;
    struct gd d;
    if (read_gd(s, g, &d))
        return 0;
    return boff(s, d.inode_table) + (uint64_t)((ino - 1) % s->ipg) * s->isize;
}

static int read_inode(struct sb *s, uint32_t ino, struct inode *in)
{
    uint64_t off = inode_off(s, ino);
    return off ? rd(s, off, in, sizeof *in) : ZK_EIO;
}

static int write_inode(struct enode *n)
{
    uint64_t off = inode_off(n->sb, n->ino);
    return off ? wr(n->sb, off, &n->in, sizeof n->in) : ZK_EIO;
}

static uint64_t isize(const struct inode *in)
{
    return in->size | ((in->mode & S_IFMT) == S_IFREG ? (uint64_t)in->size_high << 32 : 0);
}

static void set_isize(struct inode *in, uint64_t size)
{
    in->size = (uint32_t)size;
    if ((in->mode & S_IFMT) == S_IFREG)
        in->size_high = (uint32_t)(size >> 32);
}

// block mapping

static uint32_t sectors_per_block(struct sb *s)
{
    return s->bsize / 512;
}

// the slot in an indirect block, allocating the block if asked
static uint32_t indirect(struct enode *n, uint32_t *ptr, uint32_t index, int alloc, bool *dirty)
{
    struct sb *s = n->sb;
    if (!*ptr) {
        if (!alloc)
            return 0;
        *ptr = alloc_block(s, (n->ino - 1) / s->ipg);
        if (!*ptr)
            return 0;
        n->in.blocks += sectors_per_block(s);
        *dirty = true;
    }
    uint64_t off = boff(s, *ptr) + (uint64_t)index * 4;
    uint32_t v = rd32(s, off);
    if (!v && alloc) {
        v = alloc_block_z(s, (n->ino - 1) / s->ipg, alloc != 2);
        if (!v)
            return 0;
        n->in.blocks += sectors_per_block(s);
        wr(s, off, &v, 4);
        *dirty = true;
    }
    return v;
}

// file block -> disk block, 0 for a hole (or failure when allocating).
// alloc 2 means the caller overwrites the whole block, so skip clearing it
static uint32_t bmap(struct enode *n, uint32_t fb, int alloc)
{
    struct sb *s = n->sb;
    uint32_t per = s->bsize / 4;
    bool dirty = false;
    uint32_t r = 0;
    if (fb < NDIR) {
        if (!n->in.block[fb] && alloc) {
            n->in.block[fb] = alloc_block_z(s, (n->ino - 1) / s->ipg, alloc != 2);
            if (n->in.block[fb]) {
                n->in.blocks += sectors_per_block(s);
                dirty = true;
            }
        }
        r = n->in.block[fb];
    } else if ((fb -= NDIR) < per) {
        r = indirect(n, &n->in.block[12], fb, alloc, &dirty);
    } else if ((fb -= per) < per * per) {
        uint32_t mid = indirect(n, &n->in.block[13], fb / per, alloc ? 1 : 0, &dirty);
        r = mid ? indirect(n, &mid, fb % per, alloc, &dirty) : 0;
    } else {
        fb -= per * per;
        uint32_t a = indirect(n, &n->in.block[14], fb / (per * per), alloc ? 1 : 0, &dirty);
        uint32_t b = a ? indirect(n, &a, (fb / per) % per, alloc ? 1 : 0, &dirty) : 0;
        r = b ? indirect(n, &b, fb % per, alloc, &dirty) : 0;
    }
    if (dirty)
        write_inode(n);
    return r;
}

// frees an indirect tree, keeping the first `keep` data blocks under it
static void free_tree(struct sb *s, uint32_t block, int depth, uint64_t base, uint64_t keep,
                      struct inode *in)
{
    if (!block)
        return;
    uint32_t per = s->bsize / 4;
    uint64_t span = 1;
    for (int i = 1; i < depth; i++)
        span *= per;
    uint32_t *ptrs = kmalloc(s->bsize);
    if (!ptrs)
        return;
    rd(s, boff(s, block), ptrs, s->bsize);
    bool changed = false;
    for (uint32_t i = 0; i < per; i++) {
        uint64_t first = base + i * span;
        if (!ptrs[i] || first + span <= keep)
            continue;
        if (depth == 1) {
            if (first >= keep) {
                free_block(s, ptrs[i]);
                in->blocks -= s->bsize / 512;
                ptrs[i] = 0;
                changed = true;
            }
        } else {
            free_tree(s, ptrs[i], depth - 1, first, keep, in);
            if (first >= keep) {
                ptrs[i] = 0;
                changed = true;
            }
        }
    }
    if (changed)
        wr(s, boff(s, block), ptrs, s->bsize);
    kfree(ptrs);
    if (base >= keep) {
        free_block(s, block);
        in->blocks -= s->bsize / 512;
    }
}

static void truncate_blocks(struct enode *n, uint64_t size)
{
    struct sb *s = n->sb;
    uint64_t keep = (size + s->bsize - 1) / s->bsize;
    uint32_t per = s->bsize / 4;
    // fast symlinks keep their target in the block array
    if ((n->in.mode & S_IFMT) == S_IFLNK && n->in.blocks == 0)
        return;
    for (uint32_t i = 0; i < NDIR; i++) {
        if (i >= keep && n->in.block[i]) {
            free_block(s, n->in.block[i]);
            n->in.blocks -= sectors_per_block(s);
            n->in.block[i] = 0;
        }
    }
    uint64_t base = NDIR;
    for (int depth = 1; depth <= 3; depth++) {
        uint32_t *slot = &n->in.block[11 + depth];
        uint64_t span = per;
        for (int i = 1; i < depth; i++)
            span *= per;
        if (*slot) {
            free_tree(s, *slot, depth, base, keep, &n->in);
            if (base >= keep)
                *slot = 0;
        }
        base += span;
    }
}

// nodes

static enum vnode_type vtype(uint16_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return VNODE_DIR;
    case S_IFLNK: return VNODE_SYMLINK;
    default:      return VNODE_FILE;
    }
}

static void sync_vnode(struct enode *n)
{
    n->v.size = isize(&n->in);
    n->v.mode = n->in.mode & 07777;
    n->v.uid = n->in.uid | (uint32_t)(n->in.osd2[4] | n->in.osd2[5] << 8) << 16;
    n->v.gid = n->in.gid | (uint32_t)(n->in.osd2[6] | n->in.osd2[7] << 8) << 16;
    n->v.mtime = n->in.mtime;
}

// lock held
static struct enode *get_node(struct sb *s, uint32_t ino)
{
    for (struct enode *n = s->nodes; n; n = n->next) {
        if (n->ino == ino) {
            vnode_get(&n->v);
            return n;
        }
    }
    struct enode *n = kzalloc(sizeof(*n));
    if (!n)
        return NULL;
    if (read_inode(s, ino, &n->in)) {
        kfree(n);
        return NULL;
    }
    n->sb = s;
    n->ino = ino;
    n->v = (struct vnode){ .type = vtype(n->in.mode), .refs = 1, .ino = ino, .ops = &ops };
    sync_vnode(n);
    n->next = s->nodes;
    s->nodes = n;
    return n;
}

static void touch(struct enode *n)
{
    n->in.mtime = n->in.ctime = (uint32_t)rtc_now();
    n->v.mtime = n->in.mtime;
}

// file data

static int64_t e_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct enode *n = EN(v);
    struct sb *s = n->sb;
    mutex_lock(&s->lock);
    uint64_t size = isize(&n->in);
    int64_t done = 0;
    if ((n->in.mode & S_IFMT) == S_IFLNK && n->in.blocks == 0) {
        // fast symlink
        if (off < size) {
            size_t c = size - off < len ? (size_t)(size - off) : len;
            memcpy(buf, (const char *)n->in.block + off, c);
            done = (int64_t)c;
        }
        mutex_unlock(&s->lock);
        return done;
    }
    uint8_t *out = buf;
    while (off < size && (size_t)done < len) {
        uint32_t fb = (uint32_t)(off / s->bsize);
        size_t in = off % s->bsize;
        size_t c = s->bsize - in;
        if (c > len - (size_t)done)
            c = len - (size_t)done;
        if (c > size - off)
            c = (size_t)(size - off);
        uint32_t block = bmap(n, fb, false);
        int err = 0;
        if (block)
            err = rd(s, boff(s, block) + in, out + done, c);
        else
            memset(out + done, 0, c);
        if (err) {
            mutex_unlock(&s->lock);
            return done ? done : err;
        }
        done += (int64_t)c;
        off += c;
    }
    mutex_unlock(&s->lock);
    return done;
}

static int64_t e_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    struct enode *n = EN(v);
    struct sb *s = n->sb;
    if (s->readonly)
        return ZK_EROFS;
    if (len == 0)
        return 0;
    mutex_lock(&s->lock);
    // short symlink targets live in the inode
    if ((n->in.mode & S_IFMT) == S_IFLNK && off == 0 && len < 60 && n->in.blocks == 0) {
        memset(n->in.block, 0, sizeof n->in.block);
        memcpy(n->in.block, buf, len);
        set_isize(&n->in, len);
        touch(n);
        write_inode(n);
        sync_vnode(n);
        mutex_unlock(&s->lock);
        return (int64_t)len;
    }
    const uint8_t *in = buf;
    size_t done = 0;
    int err = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint32_t fb = (uint32_t)(pos / s->bsize);
        size_t o = pos % s->bsize;
        size_t c = s->bsize - o < len - done ? s->bsize - o : len - done;
        uint32_t block = bmap(n, fb, c == s->bsize ? 2 : 1);
        if (!block) {
            err = ZK_ENOSPC;
            break;
        }
        err = wr(s, boff(s, block) + o, in + done, c);
        if (err)
            break;
        done += c;
    }
    if (off + done > isize(&n->in))
        set_isize(&n->in, off + done);
    touch(n);
    write_inode(n);
    sync_vnode(n);
    mutex_unlock(&s->lock);
    return done ? (int64_t)done : err;
}

static int e_truncate(struct vnode *v, uint64_t size)
{
    struct enode *n = EN(v);
    struct sb *s = n->sb;
    if (s->readonly)
        return ZK_EROFS;
    mutex_lock(&s->lock);
    if (size < isize(&n->in))
        truncate_blocks(n, size);
    // growing leaves a hole, which reads back as zeros
    set_isize(&n->in, size);
    touch(n);
    write_inode(n);
    sync_vnode(n);
    mutex_unlock(&s->lock);
    return 0;
}

// directories

// calls fn for each live entry, stops when it returns true. *off gets the
// entry's byte offset in the dir, *prev the one before it in the same block
typedef bool (*dir_fn)(struct dirent2 *d, void *ctx);

static int each_entry(struct enode *dir, dir_fn fn, void *ctx, uint64_t *off, uint64_t *prev)
{
    struct sb *s = dir->sb;
    uint64_t size = isize(&dir->in);
    uint8_t *buf = kmalloc(s->bsize);
    if (!buf)
        return ZK_ENOMEM;
    int hit = 0;
    for (uint64_t pos = 0; pos < size && !hit; pos += s->bsize) {
        uint32_t block = bmap(dir, (uint32_t)(pos / s->bsize), false);
        if (!block || rd(s, boff(s, block), buf, s->bsize))
            continue;
        uint32_t last = UINT32_MAX;
        for (uint32_t i = 0; i + 8 <= s->bsize; ) {
            struct dirent2 *d = (void *)(buf + i);
            if (d->rec_len < 8 || i + d->rec_len > s->bsize)
                break;
            if (d->inode && fn(d, ctx)) {
                if (off)
                    *off = pos + i;
                if (prev)
                    *prev = last == UINT32_MAX ? UINT64_MAX : pos + last;
                hit = 1;
                break;
            }
            last = i;
            i += d->rec_len;
        }
    }
    kfree(buf);
    return hit;
}

struct name_q {
    const char *name;
    uint32_t ino;
    uint8_t type;
};

static bool by_name(struct dirent2 *d, void *ctx)
{
    struct name_q *q = ctx;
    size_t l = strlen(q->name);
    if (d->name_len == l && memcmp(d->name, q->name, l) == 0) {
        q->ino = d->inode;
        q->type = d->file_type;
        return true;
    }
    return false;
}

static int e_lookup(struct vnode *dv, const char *name, struct vnode **out)
{
    struct enode *dir = EN(dv);
    struct sb *s = dir->sb;
    mutex_lock(&s->lock);
    struct name_q q = { name, 0, 0 };
    int r = each_entry(dir, by_name, &q, NULL, NULL);
    struct enode *n = r == 1 ? get_node(s, q.ino) : NULL;
    mutex_unlock(&s->lock);
    if (r < 0)
        return r;
    if (r == 0)
        return ZK_ENOENT;
    if (!n)
        return ZK_EIO;
    *out = &n->v;
    return 0;
}

static uint16_t rec_size(size_t name_len)
{
    return (uint16_t)((8 + name_len + 3) & ~3UL);
}

static uint8_t ftype(uint16_t mode)
{
    switch (mode & S_IFMT) {
    case S_IFDIR: return FT_DIR;
    case S_IFLNK: return FT_LNK;
    default:      return FT_REG;
    }
}

// lock held. adds name -> ino, splitting an entry with room or growing the dir
static int add_entry(struct enode *dir, const char *name, uint32_t ino, uint8_t type)
{
    struct sb *s = dir->sb;
    size_t nl = strlen(name);
    uint16_t need = rec_size(nl);
    uint8_t *buf = kmalloc(s->bsize);
    if (!buf)
        return ZK_ENOMEM;
    uint64_t size = isize(&dir->in);
    for (uint64_t pos = 0; pos < size; pos += s->bsize) {
        uint32_t block = bmap(dir, (uint32_t)(pos / s->bsize), false);
        if (!block || rd(s, boff(s, block), buf, s->bsize))
            continue;
        for (uint32_t i = 0; i + 8 <= s->bsize; ) {
            struct dirent2 *d = (void *)(buf + i);
            if (d->rec_len < 8 || i + d->rec_len > s->bsize)
                break;
            uint16_t used = d->inode ? rec_size(d->name_len) : 0;
            if (d->rec_len - used >= need) {
                struct dirent2 *e = d;
                if (d->inode) {
                    e = (void *)(buf + i + used);
                    e->rec_len = (uint16_t)(d->rec_len - used);
                    d->rec_len = used;
                }
                e->inode = ino;
                e->name_len = (uint8_t)nl;
                e->file_type = type;
                memcpy(e->name, name, nl);
                int err = wr(s, boff(s, block), buf, s->bsize);
                kfree(buf);
                touch(dir);
                dir->in.flags &= ~INDEX_FL;
                write_inode(dir);
                return err;
            }
            i += d->rec_len;
        }
    }
    // a fresh block at the end
    uint32_t block = bmap(dir, (uint32_t)(size / s->bsize), true);
    if (!block) {
        kfree(buf);
        return ZK_ENOSPC;
    }
    memset(buf, 0, s->bsize);
    struct dirent2 *e = (void *)buf;
    e->inode = ino;
    e->rec_len = (uint16_t)s->bsize;
    e->name_len = (uint8_t)nl;
    e->file_type = type;
    memcpy(e->name, name, nl);
    int err = wr(s, boff(s, block), buf, s->bsize);
    kfree(buf);
    set_isize(&dir->in, size + s->bsize);
    touch(dir);
    dir->in.flags &= ~INDEX_FL;
    write_inode(dir);
    sync_vnode(dir);
    return err;
}

// lock held. takes name out of dir, merging its space into the entry before
static int remove_entry(struct enode *dir, const char *name)
{
    struct sb *s = dir->sb;
    struct name_q q = { name, 0, 0 };
    uint64_t off, prev;
    int r = each_entry(dir, by_name, &q, &off, &prev);
    if (r <= 0)
        return r < 0 ? r : ZK_ENOENT;
    uint32_t block = bmap(dir, (uint32_t)(off / s->bsize), false);
    uint64_t base = boff(s, block);
    struct dirent2 d;
    rd(s, base + off % s->bsize, &d, 8);
    if (prev != UINT64_MAX) {
        struct dirent2 p;
        rd(s, base + prev % s->bsize, &p, 8);
        p.rec_len = (uint16_t)(p.rec_len + d.rec_len);
        wr(s, base + prev % s->bsize + 4, &p.rec_len, 2);
    } else {
        uint32_t zero = 0;
        wr(s, base + off % s->bsize, &zero, 4);
    }
    touch(dir);
    write_inode(dir);
    return 0;
}

struct nth_q {
    size_t want, seen;
    struct zk_dirent *out;
};

static bool nth(struct dirent2 *d, void *ctx)
{
    struct nth_q *q = ctx;
    if ((d->name_len == 1 && d->name[0] == '.') ||
        (d->name_len == 2 && d->name[0] == '.' && d->name[1] == '.'))
        return false;
    if (q->seen++ != q->want)
        return false;
    size_t n = d->name_len < ZK_NAME_MAX ? d->name_len : ZK_NAME_MAX;
    memcpy(q->out->name, d->name, n);
    q->out->name[n] = '\0';
    q->out->type = d->file_type == FT_DIR ? ZK_TYPE_DIR : d->file_type == FT_LNK ? ZK_TYPE_LINK :
                   ZK_TYPE_FILE;
    return true;
}

static int e_readdir(struct vnode *dv, size_t index, struct zk_dirent *out)
{
    struct enode *dir = EN(dv);
    mutex_lock(&dir->sb->lock);
    struct nth_q q = { index, 0, out };
    int r = each_entry(dir, nth, &q, NULL, NULL);
    mutex_unlock(&dir->sb->lock);
    return r;
}

static int e_create(struct vnode *dv, const char *name, enum vnode_type type, struct vnode **out)
{
    struct enode *dir = EN(dv);
    struct sb *s = dir->sb;
    if (s->readonly)
        return ZK_EROFS;
    if (strlen(name) > 255)
        return ZK_ENAMETOOLONG;
    if (type != VNODE_FILE && type != VNODE_DIR && type != VNODE_SYMLINK)
        return ZK_EPERM;
    mutex_lock(&s->lock);
    struct name_q q = { name, 0, 0 };
    int err = 0;
    if (dir->unlinked)
        err = ZK_ENOENT;
    else if (each_entry(dir, by_name, &q, NULL, NULL) == 1)
        err = ZK_EEXIST;
    uint32_t ino = err ? 0 : alloc_inode(s, type == VNODE_DIR);
    if (!err && !ino)
        err = ZK_ENOSPC;
    if (err) {
        mutex_unlock(&s->lock);
        return err;
    }

    struct inode in;
    memset(&in, 0, sizeof in);
    in.mode = (uint16_t)(type == VNODE_DIR ? S_IFDIR | 0755 : type == VNODE_SYMLINK ? S_IFLNK | 0777 :
                         S_IFREG | 0644);
    in.links = type == VNODE_DIR ? 2 : 1;
    in.atime = in.ctime = in.mtime = (uint32_t)rtc_now();
    uint64_t off = inode_off(s, ino);
    // the whole on-disk inode, extra space included, starts zeroed
    uint8_t zero[256] = { 0 };
    wr(s, off, zero, s->isize < sizeof zero ? s->isize : sizeof zero);
    wr(s, off, &in, sizeof in);

    struct enode *n = get_node(s, ino);
    if (!n) {
        free_inode(s, ino, type == VNODE_DIR);
        mutex_unlock(&s->lock);
        return ZK_ENOMEM;
    }
    if (type == VNODE_DIR) {
        // . and ..
        uint32_t block = bmap(n, 0, true);
        uint8_t *buf = block ? kzalloc(s->bsize) : NULL;
        if (buf) {
            struct dirent2 *d = (void *)buf;
            d->inode = ino;
            d->rec_len = 12;
            d->name_len = 1;
            d->file_type = FT_DIR;
            d->name[0] = '.';
            d = (void *)(buf + 12);
            d->inode = dir->ino;
            d->rec_len = (uint16_t)(s->bsize - 12);
            d->name_len = 2;
            d->file_type = FT_DIR;
            d->name[0] = d->name[1] = '.';
            wr(s, boff(s, block), buf, s->bsize);
            kfree(buf);
        }
        set_isize(&n->in, s->bsize);
        write_inode(n);
        sync_vnode(n);
        dir->in.links++;
        write_inode(dir);
    }
    err = add_entry(dir, name, ino, ftype(n->in.mode));
    mutex_unlock(&s->lock);
    if (err) {
        // leave it for fsck to find rather than half undo it
        vnode_put(&n->v);
        return err;
    }
    *out = &n->v;
    return 0;
}

static bool dir_empty(struct enode *n)
{
    struct nth_q q = { 0, 0, &(struct zk_dirent){ 0 } };
    return each_entry(n, nth, &q, NULL, NULL) == 0;
}

// lock held. drops one link, frees the inode now or once the last user is gone
static void drop_link(struct enode *n)
{
    bool dir = (n->in.mode & S_IFMT) == S_IFDIR;
    if (dir)
        n->in.links = 0;
    else if (n->in.links)
        n->in.links--;
    if (n->in.links == 0) {
        n->unlinked = true;
        n->in.dtime = (uint32_t)rtc_now();
    }
    write_inode(n);
}

static const struct vnode_ops ops;

// a hard link to a file on the same filesystem
static int e_link(struct vnode *dv, const char *name, struct vnode *v)
{
    struct enode *dir = EN(dv);
    struct sb *s = dir->sb;
    if (v->ops != &ops)
        return ZK_EPERM;    // devices and the like can't live on disk
    struct enode *n = EN(v);
    if (n->sb != s)
        return ZK_EXDEV;
    if (s->readonly)
        return ZK_EROFS;
    if ((n->in.mode & S_IFMT) == S_IFDIR)
        return ZK_EPERM;
    if (strlen(name) > 255)
        return ZK_ENAMETOOLONG;
    mutex_lock(&s->lock);
    struct name_q q = { name, 0, 0 };
    int err = 0;
    if (dir->unlinked || n->unlinked)
        err = ZK_ENOENT;
    else if (each_entry(dir, by_name, &q, NULL, NULL) == 1)
        err = ZK_EEXIST;
    else if (n->in.links >= 65000)
        err = ZK_EMFILE;
    if (!err)
        err = add_entry(dir, name, n->ino, ftype(n->in.mode));
    if (!err) {
        n->in.links++;
        n->in.ctime = (uint32_t)rtc_now();
        write_inode(n);
    }
    mutex_unlock(&s->lock);
    return err;
}

static int e_unlink(struct vnode *dv, const char *name)
{
    struct enode *dir = EN(dv);
    struct sb *s = dir->sb;
    if (s->readonly)
        return ZK_EROFS;
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return ZK_EINVAL;
    mutex_lock(&s->lock);
    struct name_q q = { name, 0, 0 };
    int r = each_entry(dir, by_name, &q, NULL, NULL);
    struct enode *n = r == 1 ? get_node(s, q.ino) : NULL;
    int err = r < 0 ? r : !n ? ZK_ENOENT : 0;
    if (!err && (n->in.mode & S_IFMT) == S_IFDIR && !dir_empty(n))
        err = ZK_ENOTEMPTY;
    if (!err)
        err = remove_entry(dir, name);
    if (!err) {
        if ((n->in.mode & S_IFMT) == S_IFDIR && dir->in.links > 2) {
            dir->in.links--;
            write_inode(dir);
        }
        drop_link(n);
    }
    mutex_unlock(&s->lock);
    if (n)
        vnode_put(&n->v);
    return err;
}

static int e_rename(struct vnode *odv, const char *oname, struct vnode *ndv, const char *nname)
{
    struct enode *od = EN(odv), *nd = EN(ndv);
    struct sb *s = od->sb;
    if (nd->sb != s)
        return ZK_EXDEV;
    if (s->readonly)
        return ZK_EROFS;
    mutex_lock(&s->lock);
    struct name_q q = { oname, 0, 0 };
    int r = each_entry(od, by_name, &q, NULL, NULL);
    struct enode *n = r == 1 ? get_node(s, q.ino) : NULL;
    struct enode *victim = NULL;
    int err = r < 0 ? r : !n ? ZK_ENOENT : 0;
    bool is_dir = n && (n->in.mode & S_IFMT) == S_IFDIR;
    if (!err) {
        struct name_q t = { nname, 0, 0 };
        if (each_entry(nd, by_name, &t, NULL, NULL) == 1) {
            if (t.ino == n->ino)
                goto out;
            victim = get_node(s, t.ino);
            bool vdir = victim && (victim->in.mode & S_IFMT) == S_IFDIR;
            if (!victim)
                err = ZK_EIO;
            else if (is_dir && !vdir)
                err = ZK_ENOTDIR;
            else if (!is_dir && vdir)
                err = ZK_EISDIR;
            else if (vdir && !dir_empty(victim))
                err = ZK_ENOTEMPTY;
            if (!err) {
                err = remove_entry(nd, nname);
                if (!err) {
                    if (vdir && nd->in.links > 2)
                        nd->in.links--;
                    drop_link(victim);
                }
            }
        }
    }
    if (!err)
        err = add_entry(nd, nname, n->ino, ftype(n->in.mode));
    if (!err)
        err = remove_entry(od, oname);
    if (!err && is_dir && od != nd) {
        // its .. now points at the new parent
        uint32_t block = bmap(n, 0, false);
        if (block)
            wr(s, boff(s, block) + 12, &nd->ino, 4);
        od->in.links--;
        nd->in.links++;
        write_inode(od);
        write_inode(nd);
    }
out:
    mutex_unlock(&s->lock);
    if (n)
        vnode_put(&n->v);
    if (victim)
        vnode_put(&victim->v);
    return err;
}

static int e_setattr(struct vnode *v, uint32_t uid, uint32_t gid, uint32_t mode)
{
    struct enode *n = EN(v);
    struct sb *s = n->sb;
    if (s->readonly)
        return ZK_EROFS;
    mutex_lock(&s->lock);
    n->in.uid = (uint16_t)uid;
    n->in.gid = (uint16_t)gid;
    n->in.osd2[4] = (uint8_t)(uid >> 16);
    n->in.osd2[5] = (uint8_t)(uid >> 24);
    n->in.osd2[6] = (uint8_t)(gid >> 16);
    n->in.osd2[7] = (uint8_t)(gid >> 24);
    n->in.mode = (uint16_t)((n->in.mode & S_IFMT) | (mode & 07777));
    n->in.ctime = (uint32_t)rtc_now();
    write_inode(n);
    sync_vnode(n);
    mutex_unlock(&s->lock);
    return 0;
}

static void e_release(struct vnode *v)
{
    struct enode *n = EN(v);
    struct sb *s = n->sb;
    mutex_lock(&s->lock);
    for (struct enode **pp = &s->nodes; *pp; pp = &(*pp)->next) {
        if (*pp == n) {
            *pp = n->next;
            break;
        }
    }
    if (n->unlinked && !s->readonly) {
        truncate_blocks(n, 0);
        set_isize(&n->in, 0);
        write_inode(n);
        free_inode(s, n->ino, (n->in.mode & S_IFMT) == S_IFDIR);
    }
    bool free_sb = n == s->root && s->unmounted;
    mutex_unlock(&s->lock);
    kfree(n);
    if (free_sb)
        kfree(s);
}

static const struct vnode_ops ops = {
    .read = e_read,
    .write = e_write,
    .truncate = e_truncate,
    .lookup = e_lookup,
    .create = e_create,
    .link = e_link,
    .unlink = e_unlink,
    .readdir = e_readdir,
    .rename = e_rename,
    .setattr = e_setattr,
    .release = e_release,
};

static int e_statfs(struct mount *m, struct zk_statfs *st)
{
    struct sb *s = m->sb;
    st->block_size = s->bsize;
    st->blocks = s->blocks - s->first_data;
    st->blocks_free = s->free_blocks;
    return 0;
}

static int e_unmount(struct mount *m)
{
    struct sb *s = m->sb;
    mutex_lock(&s->lock);
    if (s->nodes != s->root || s->root->next || s->root->v.refs > 1) {
        mutex_unlock(&s->lock);
        return ZK_EBUSY;
    }
    // mark it clean
    uint16_t state = 1;
    if (!s->readonly)
        wr(s, 1024 + 58, &state, 2);
    s->unmounted = true;
    mutex_unlock(&s->lock);
    return 0;
}

static int e_sync(struct mount *m)
{
    (void)m;
    return 0;    // everything is written through already
}

static bool has_opt(const char *opts, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = opts; p && *p; ) {
        if (strncmp(p, word, n) == 0 && (p[n] == ',' || !p[n]))
            return true;
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return false;
}

static int ext2_mount(struct mount *m, const char *source, const char *opts)
{
    struct blkdev *dev = blk_find(source);
    if (!dev)
        return ZK_ENODEV;
    uint8_t *raw = kmalloc(1024);
    struct sb *s = kzalloc(sizeof(*s));
    if (!raw || !s) {
        kfree(raw);
        kfree(s);
        return ZK_ENOMEM;
    }
    s->dev = dev;
    int err = blk_read(dev, 1024, raw, 1024);
    uint32_t *w = (uint32_t *)raw;
    uint16_t magic = (uint16_t)(raw[56] | raw[57] << 8);
    if (!err && magic != EXT2_MAGIC)
        err = ZK_EINVAL;
    if (err) {
        kfree(raw);
        kfree(s);
        return err;
    }
    s->inodes = w[0];
    s->blocks = w[1];
    s->free_blocks = w[3];
    s->free_inodes = w[4];
    s->first_data = w[5];
    s->bsize = 1024U << w[6];
    s->bpg = w[8];
    s->ipg = w[10];
    uint32_t rev = w[19];
    s->isize = rev >= 1 ? (uint16_t)(raw[88] | raw[89] << 8) : 128;
    uint32_t incompat = rev >= 1 ? w[24] : 0, ro_compat = rev >= 1 ? w[25] : 0;
    kfree(raw);
    if (s->bsize > 4096 || !s->bpg || !s->ipg || s->isize < 128) {
        kfree(s);
        return ZK_EINVAL;
    }
    if (incompat & ~INCOMPAT_FILETYPE) {
        pr_warn("ext2: %s uses features this driver doesn't know (%#x)\n", source, incompat);
        kfree(s);
        return ZK_EINVAL;
    }
    s->readonly = (ro_compat & ~(RO_COMPAT_SPARSE | RO_COMPAT_LARGE)) != 0 || has_opt(opts, "ro");
    s->ngroups = (s->blocks - s->first_data + s->bpg - 1) / s->bpg;
    s->gdt = (uint64_t)(s->first_data + 1) * s->bsize;

    struct enode *root = get_node(s, ROOT_INO);
    if (!root || root->v.type != VNODE_DIR) {
        kfree(root);
        kfree(s);
        return ZK_EINVAL;
    }
    s->root = root;
    m->sb = s;
    m->root = &root->v;
    m->statfs = e_statfs;
    m->unmount = e_unmount;
    m->sync = e_sync;
    if (!s->readonly) {
        uint16_t state = 2;    // in use, not cleanly unmounted
        wr(s, 1024 + 58, &state, 2);
    }
    return 0;
}

const struct fs_type ext2_type = { "ext2", ext2_mount };

// bind: an existing directory shows up somewhere else too

static int bind_mount(struct mount *m, const char *source, const char *opts)
{
    (void)opts;
    struct vnode *v;
    int err = vfs_lookup(source, &v);
    if (err)
        return err;
    if (v->type != VNODE_DIR) {
        vnode_put(v);
        return ZK_ENOTDIR;
    }
    m->root = v;
    return 0;
}

const struct fs_type bind_type = { "bind", bind_mount };
