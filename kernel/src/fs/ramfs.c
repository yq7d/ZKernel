// in-memory filesystem, the root of the tree
#include "fs/vfs.h"
#include "lib/string.h"
#include "drivers/rtc.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "sched/sync.h"

#define MAX_FILE_SIZE (256UL << 20)

struct entry {
    char name[ZK_NAME_MAX + 1];
    struct vnode *v;
    struct entry *next;
};

struct rnode {
    struct vnode v;
    uint8_t *data;
    size_t cap;
    struct entry *entries;
    bool dead;    // dir was unlinked
};

static struct mutex lock = MUTEX_INIT;
static size_t bytes_used;
static const struct vnode_ops ops;

#define RNODE(vn) ((struct rnode *)(vn))

size_t ramfs_bytes_used(void)
{
    return bytes_used;
}

static struct rnode *new_node(enum vnode_type type)
{
    struct rnode *r = kzalloc(sizeof(*r));
    if (!r)
        return NULL;
    r->v.type = type;
    r->v.refs = 1;
    r->v.ino = vfs_next_ino();
    r->v.ops = &ops;
    r->v.mtime = rtc_now();
    r->v.mode = type == VNODE_DIR ? 0755 : 0644;
    return r;
}

static struct entry *find(struct rnode *dir, const char *name)
{
    for (struct entry *e = dir->entries; e; e = e->next)
        if (strcmp(e->name, name) == 0)
            return e;
    return NULL;
}

static int64_t rd(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct rnode *r = RNODE(v);
    mutex_lock(&lock);
    int64_t n = 0;
    if (off < v->size) {
        n = (int64_t)(v->size - off < len ? v->size - off : len);
        memcpy(buf, r->data + off, (size_t)n);
    }
    mutex_unlock(&lock);
    return n;
}

// lock held
static int grow(struct rnode *r, size_t need)
{
    if (need <= r->cap)
        return 0;
    if (need > MAX_FILE_SIZE)
        return ZK_EFBIG;
    size_t cap = r->cap ? r->cap : 64;
    while (cap < need)
        cap *= 2;
    uint8_t *d = krealloc(r->data, cap);
    if (!d)
        return ZK_ENOMEM;
    bytes_used += cap - r->cap;
    r->data = d;
    r->cap = cap;
    return 0;
}

static int64_t wr(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    struct rnode *r = RNODE(v);
    if (len == 0)
        return 0;
    if (off > MAX_FILE_SIZE || len > MAX_FILE_SIZE - off)
        return ZK_EFBIG;

    mutex_lock(&lock);
    int err = grow(r, off + len);
    if (err) {
        mutex_unlock(&lock);
        return err;
    }
    // writing past the end leaves a zeroed hole
    if (off > v->size)
        memset(r->data + v->size, 0, off - v->size);
    memcpy(r->data + off, buf, len);
    if (off + len > v->size)
        v->size = off + len;
    v->mtime = rtc_now();
    mutex_unlock(&lock);
    return (int64_t)len;
}

static int truncate(struct vnode *v, uint64_t size)
{
    struct rnode *r = RNODE(v);
    mutex_lock(&lock);
    int err = 0;
    if (size > v->size) {
        err = grow(r, size);
        if (!err)
            memset(r->data + v->size, 0, size - v->size);
    }
    if (!err) {
        v->size = size;
        v->mtime = rtc_now();
    }
    mutex_unlock(&lock);
    return err;
}

static int lookup(struct vnode *dir, const char *name, struct vnode **out)
{
    mutex_lock(&lock);
    struct entry *e = find(RNODE(dir), name);
    if (e) {
        vnode_get(e->v);
        *out = e->v;
    }
    mutex_unlock(&lock);
    return e ? 0 : ZK_ENOENT;
}

// lock held, takes a reference to v for the entry
static int add_entry(struct rnode *dir, const char *name, struct vnode *v)
{
    if (dir->dead)
        return ZK_ENOENT;
    if (find(dir, name))
        return ZK_EEXIST;
    struct entry *e = kzalloc(sizeof(*e));
    if (!e)
        return ZK_ENOMEM;
    strlcpy(e->name, name, sizeof e->name);
    vnode_get(v);
    e->v = v;

    // keep entries sorted by name
    struct entry **p = &dir->entries;
    while (*p && strcmp((*p)->name, name) < 0)
        p = &(*p)->next;
    e->next = *p;
    *p = e;
    dir->v.size++;
    return 0;
}

static int create(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out)
{
    struct rnode *r = new_node(type);
    if (!r)
        return ZK_ENOMEM;
    mutex_lock(&lock);
    int err = add_entry(RNODE(dir), name, &r->v);
    mutex_unlock(&lock);
    if (err) {
        kfree(r);
        return err;
    }
    *out = &r->v;
    return 0;
}

static int link(struct vnode *dir, const char *name, struct vnode *v)
{
    mutex_lock(&lock);
    int err = add_entry(RNODE(dir), name, v);
    mutex_unlock(&lock);
    return err;
}

static int unlink(struct vnode *dir, const char *name)
{
    struct rnode *d = RNODE(dir);
    mutex_lock(&lock);
    struct entry **p = &d->entries;
    while (*p && strcmp((*p)->name, name) != 0)
        p = &(*p)->next;
    struct entry *e = *p;
    if (!e) {
        mutex_unlock(&lock);
        return ZK_ENOENT;
    }
    struct vnode *v = e->v;
    if (v->type == VNODE_DIR) {
        if (v->ops != &ops || RNODE(v)->entries) {
            mutex_unlock(&lock);
            return ZK_ENOTEMPTY;
        }
        RNODE(v)->dead = true;
    }
    *p = e->next;
    d->v.size--;
    mutex_unlock(&lock);

    kfree(e);
    vnode_put(v);
    return 0;
}

static int readdir(struct vnode *dir, size_t index, struct zk_dirent *out)
{
    mutex_lock(&lock);
    struct entry *e = RNODE(dir)->entries;
    while (e && index--)
        e = e->next;
    if (e) {
        out->type = e->v->type;
        strlcpy(out->name, e->name, sizeof out->name);
    }
    mutex_unlock(&lock);
    return e ? 1 : 0;
}

// lock held. unlinks name from dir and returns the entry
static struct entry *take(struct rnode *d, const char *name)
{
    struct entry **p = &d->entries;
    while (*p && strcmp((*p)->name, name) != 0)
        p = &(*p)->next;
    struct entry *e = *p;
    if (e) {
        *p = e->next;
        d->v.size--;
    }
    return e;
}

static int rename(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname)
{
    struct rnode *od = RNODE(odir), *nd = RNODE(ndir);
    mutex_lock(&lock);
    struct entry *e = find(od, oname);
    if (!e || nd->dead) {
        mutex_unlock(&lock);
        return ZK_ENOENT;
    }
    struct entry *old = find(nd, nname);
    struct vnode *victim = NULL;
    if (old) {
        bool src_dir = e->v->type == VNODE_DIR, dst_dir = old->v->type == VNODE_DIR;
        int err = 0;
        if (src_dir && !dst_dir)
            err = ZK_ENOTDIR;
        else if (!src_dir && dst_dir)
            err = ZK_EISDIR;
        else if (dst_dir && (old->v->ops != &ops || RNODE(old->v)->entries))
            err = ZK_ENOTEMPTY;
        if (err) {
            mutex_unlock(&lock);
            return err;
        }
        if (dst_dir)
            RNODE(old->v)->dead = true;
        take(nd, nname);
        victim = old->v;
        kfree(old);
    }

    // move the entry itself, so this can't fail halfway
    e = take(od, oname);
    strlcpy(e->name, nname, sizeof e->name);
    struct entry **p = &nd->entries;
    while (*p && strcmp((*p)->name, nname) < 0)
        p = &(*p)->next;
    e->next = *p;
    *p = e;
    nd->v.size++;
    mutex_unlock(&lock);
    if (victim)
        vnode_put(victim);
    return 0;
}

static int setattr(struct vnode *v, uint32_t uid, uint32_t gid, uint32_t mode)
{
    v->uid = uid;
    v->gid = gid;
    v->mode = mode;
    return 0;
}

static void release(struct vnode *v)
{
    struct rnode *r = RNODE(v);
    mutex_lock(&lock);
    bytes_used -= r->cap;
    struct entry *e = r->entries;
    r->entries = NULL;
    mutex_unlock(&lock);

    while (e) {
        struct entry *next = e->next;
        vnode_put(e->v);
        kfree(e);
        e = next;
    }
    kfree(r->data);
    kfree(r);
}

static const struct vnode_ops ops = {
    .read = rd,
    .write = wr,
    .truncate = truncate,
    .lookup = lookup,
    .create = create,
    .link = link,
    .unlink = unlink,
    .readdir = readdir,
    .rename = rename,
    .setattr = setattr,
    .release = release,
};

static int ramfs_statfs(struct mount *m, struct zk_statfs *st)
{
    (void)m;
    // backed by ram, so the free space is whatever ram is free
    st->block_size = PAGE_SIZE;
    st->blocks = pmm_total_pages();
    st->blocks_free = pmm_free_count();
    return 0;
}

static int ramfs_mount(struct mount *m, const char *source, const char *opts)
{
    (void)source;
    struct rnode *r = new_node(VNODE_DIR);
    if (!r)
        return ZK_ENOMEM;
    mount_opt(opts, "mode", &r->v.mode, 8);
    mount_opt(opts, "uid", &r->v.uid, 10);
    mount_opt(opts, "gid", &r->v.gid, 10);
    m->root = &r->v;
    m->statfs = ramfs_statfs;
    return 0;
}

const struct fs_type ramfs_type = { "tmpfs", ramfs_mount };
