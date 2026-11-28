#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "drivers/blk.h"
#include "lib/printf.h"
#include "panic.h"
#include "proc/proc.h"
#include "sched/sync.h"

#define MAX_FS_TYPES 8

static struct vnode *root;
static struct waitq poll_wq = WAITQ_INIT;
static uint64_t next_ino = 1;
static struct mount *mounts;
static struct mutex mount_lock = MUTEX_INIT;
static const struct fs_type *fs_types[MAX_FS_TYPES];

uint64_t vfs_next_ino(void)
{
    return __atomic_fetch_add(&next_ino, 1, __ATOMIC_RELAXED);
}

void vnode_get(struct vnode *v)
{
    __atomic_fetch_add(&v->refs, 1, __ATOMIC_RELAXED);
}

void vnode_put(struct vnode *v)
{
    if (__atomic_sub_fetch(&v->refs, 1, __ATOMIC_ACQ_REL) == 0 && v->ops->release)
        v->ops->release(v);
}

void vfs_register(const struct fs_type *type)
{
    for (int i = 0; i < MAX_FS_TYPES; i++) {
        if (!fs_types[i]) {
            fs_types[i] = type;
            return;
        }
    }
    panic("vfs: too many filesystem types");
}

static const struct fs_type *find_type(const char *name)
{
    for (int i = 0; i < MAX_FS_TYPES && fs_types[i]; i++)
        if (strcmp(fs_types[i]->name, name) == 0)
            return fs_types[i];
    return NULL;
}

int vfs_permission(struct vnode *v, int want)
{
    const struct zk_cred *c = cred_current();
    if (c->euid == 0) {
        // root still can't run something nobody may run
        if ((want & MAY_EXEC) && v->type != VNODE_DIR && !(v->mode & 0111))
            return ZK_EACCES;
        return 0;
    }
    uint32_t bits;
    if (c->euid == v->uid)
        bits = v->mode >> 6;
    else if (cred_in_group(c, v->gid))
        bits = v->mode >> 3;
    else
        bits = v->mode;
    return ((int)(bits & 7) & want) == want ? 0 : ZK_EACCES;
}

bool mount_opt(const char *opts, const char *key, uint32_t *val, int base)
{
    size_t kl = strlen(key);
    for (const char *p = opts; p && *p; ) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            uint32_t v = 0;
            for (p += kl + 1; *p && *p != ','; p++) {
                int d = *p >= '0' && *p <= '9' ? *p - '0' : -1;
                if (d < 0 || d >= base)
                    return false;
                v = v * (uint32_t)base + (uint32_t)d;
            }
            *val = v;
            return true;
        }
        p = strchr(p, ',');
        if (p)
            p++;
    }
    return false;
}

// the creator owns new files, with a fixed 022 umask
static void set_owner(struct vnode *v)
{
    if (!v->ops->setattr)
        return;
    const struct zk_cred *c = cred_current();
    v->ops->setattr(v, c->euid, c->egid, v->type == VNODE_DIR ? 0755 : 0644);
}

// in a sticky dir only the owners of the file or the dir may remove it
static int may_delete(struct vnode *dir, struct vnode *victim)
{
    int err = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
    if (err)
        return err;
    const struct zk_cred *c = cred_current();
    if ((dir->mode & ZK_S_ISVTX) && c->euid != 0 && c->euid != dir->uid &&
        c->euid != victim->uid)
        return ZK_EPERM;
    return 0;
}

void vfs_init(void)
{
    vfs_register(&ramfs_type);
    vfs_register(&procfs_type);
    vfs_register(&sysfs_type);
    vfs_register(&fat_type);
    vfs_register(&ext2_type);
    vfs_register(&bind_type);

    struct mount *m = kzalloc(sizeof(*m));
    if (!m || ramfs_type.mount(m, "", "mode=755") != 0)
        panic("vfs: can't create the root");
    strlcpy(m->path, "/", sizeof m->path);
    strlcpy(m->source, "rootfs", sizeof m->source);
    m->type = &ramfs_type;
    mounts = m;
    root = m->root;

    vfs_mkdir("/dev");
    vfs_mkdir("/tmp");
    vfs_chmod("/tmp", 01777);
    vfs_mkdir("/proc");
    vfs_mkdir("/mnt");
    vfs_mkdir("/data");
    dev_init();
    if (vfs_mount("none", "/proc", "proc", "") != 0)
        panic("vfs: can't mount /proc");
}

// if v has something mounted on it, swaps it for the mounted root
static struct vnode *cross_mount(struct vnode *v)
{
    if (v->type != VNODE_DIR)
        return v;
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next) {
        if (m->covered == v) {
            vnode_get(m->root);
            vnode_put(v);
            v = m->root;
            break;
        }
    }
    mutex_unlock(&mount_lock);
    return v;
}

static bool is_mount_point(struct vnode *v)
{
    bool found = false;
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m && !found; m = m->next)
        found = m->covered == v || m->root == v;
    mutex_unlock(&mount_lock);
    return found;
}

int vfs_mount(const char *source, const char *target, const char *fstype, const char *opts)
{
    const struct fs_type *type = find_type(fstype);
    if (!type)
        return ZK_ENODEV;
    if (strcmp(target, "/") == 0)
        return ZK_EBUSY;

    struct vnode *dir;
    int err = vfs_lookup(target, &dir);
    if (err)
        return err;
    if (dir->type != VNODE_DIR) {
        vnode_put(dir);
        return ZK_ENOTDIR;
    }
    if (is_mount_point(dir)) {
        vnode_put(dir);
        return ZK_EBUSY;
    }

    struct mount *m = kzalloc(sizeof(*m));
    if (!m) {
        vnode_put(dir);
        return ZK_ENOMEM;
    }
    strlcpy(m->path, target, sizeof m->path);
    strlcpy(m->source, source, sizeof m->source);
    m->type = type;
    strlcpy(m->opts, opts, sizeof m->opts);
    err = type->mount(m, source, opts);
    if (err) {
        vnode_put(dir);
        kfree(m);
        return err;
    }
    m->covered = dir;    // keeps the reference

    mutex_lock(&mount_lock);
    struct mount **pp = &mounts;
    while (*pp)
        pp = &(*pp)->next;
    *pp = m;
    mutex_unlock(&mount_lock);
    return 0;
}

int vfs_umount(const char *target)
{
    if (strcmp(target, "/") == 0)
        return ZK_EBUSY;
    mutex_lock(&mount_lock);
    struct mount **pp = &mounts, *m = NULL;
    for (; *pp; pp = &(*pp)->next) {
        if (strcmp((*pp)->path, target) == 0) {
            m = *pp;
            break;
        }
    }
    if (!m) {
        mutex_unlock(&mount_lock);
        return ZK_EINVAL;
    }
    // nothing may be mounted inside it
    for (struct mount *o = mounts; o; o = o->next) {
        size_t n = strlen(target);
        if (o != m && strncmp(o->path, target, n) == 0 && o->path[n] == '/') {
            mutex_unlock(&mount_lock);
            return ZK_EBUSY;
        }
    }
    int err = m->unmount ? m->unmount(m) : 0;
    if (err) {
        mutex_unlock(&mount_lock);
        return err;
    }
    blk_sync();
    *pp = m->next;
    mutex_unlock(&mount_lock);

    vnode_put(m->root);
    vnode_put(m->covered);
    kfree(m);
    return 0;
}

// the mount a path lives on: the longest matching prefix
static struct mount *mount_of(const char *path)
{
    struct mount *best = NULL;
    size_t best_len = 0;
    for (struct mount *m = mounts; m; m = m->next) {
        size_t n = strlen(m->path);
        bool match = n == 1 || (strncmp(path, m->path, n) == 0 && (path[n] == '/' || !path[n]));
        if (match && n >= best_len) {
            best = m;
            best_len = n;
        }
    }
    return best;
}

int vfs_statfs(const char *path, struct zk_statfs *st)
{
    struct zk_stat s;
    int err = vfs_stat(path, &s);
    if (err)
        return err;
    memset(st, 0, sizeof(*st));
    mutex_lock(&mount_lock);
    struct mount *m = mount_of(path);
    char source[ZK_PATH_MAX];
    bool bind = m->type == &bind_type;
    strlcpy(source, m->source, sizeof source);
    strlcpy(st->fstype, m->type->name, sizeof st->fstype);
    err = m->statfs ? m->statfs(m, st) : 0;
    mutex_unlock(&mount_lock);
    return bind ? vfs_statfs(source, st) : err;
}

void vfs_sync(void)
{
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next)
        if (m->sync)
            m->sync(m);
    mutex_unlock(&mount_lock);
    blk_sync();
}

void vfs_each_mount(void (*fn)(struct mount *m, void *ctx), void *ctx)
{
    mutex_lock(&mount_lock);
    for (struct mount *m = mounts; m; m = m->next)
        fn(m, ctx);
    mutex_unlock(&mount_lock);
}

int vfs_normalize(const char *cwd, const char *path, char *out)
{
    if (!path[0])
        return ZK_ENOENT;

    char tmp[ZK_PATH_MAX * 2];
    size_t n = 0;
    if (path[0] != '/') {
        n = strlcpy(tmp, cwd, sizeof tmp);
        if (n + 1 >= sizeof tmp)
            return ZK_ENAMETOOLONG;
        tmp[n++] = '/';
    }
    if (strlcpy(tmp + n, path, sizeof tmp - n) >= sizeof tmp - n)
        return ZK_ENAMETOOLONG;

    // out only ever shrinks on .., so remember where each component started
    size_t len = 0;
    out[0] = '\0';
    char *p = tmp;
    while (*p) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        char *start = p;
        while (*p && *p != '/')
            p++;
        size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.')
            continue;
        if (clen == 2 && start[0] == '.' && start[1] == '.') {
            while (len > 0 && out[len - 1] != '/')
                len--;
            if (len > 0)
                len--;
            out[len] = '\0';
            continue;
        }
        if (clen > ZK_NAME_MAX)
            return ZK_ENAMETOOLONG;
        if (len + 1 + clen >= ZK_PATH_MAX)
            return ZK_ENAMETOOLONG;
        out[len++] = '/';
        memcpy(out + len, start, clen);
        len += clen;
        out[len] = '\0';
    }
    if (len == 0) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

static int lookup_parent(const char *path, struct vnode **dir, char *name);

#define MAX_LINKS 16
#define COMPAT_ROOT "/compat/linux"

static int read_link(struct vnode *v, char *buf, size_t size)
{
    if (v->size >= size)
        return ZK_ENAMETOOLONG;
    int64_t n = v->ops->read(v, buf, size - 1, 0);
    if (n < 0)
        return (int)n;
    buf[n] = '\0';
    return 0;
}

// walks from the root, following symlinks. stops before the last component
// when name isn't NULL. follow says whether a symlink at the end is followed.
// absolute link targets inside /compat/linux stay inside it
static int walk(const char *path, struct vnode **out, char *name, bool follow)
{
    if (path[0] != '/')
        return ZK_EINVAL;

    char *buf = kmalloc(ZK_PATH_MAX * 3);
    if (!buf)
        return ZK_ENOMEM;
    char *todo = buf, *done = buf + ZK_PATH_MAX, *tmp = buf + ZK_PATH_MAX * 2;
    strlcpy(todo, path, ZK_PATH_MAX);

    int links = 0, err = 0;
restart:;
    struct vnode *cur = root;
    vnode_get(cur);
    done[0] = '\0';
    const char *p = todo;

    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/')
            p++;
        size_t clen = (size_t)(p - start);
        if (clen > ZK_NAME_MAX) {
            err = ZK_ENAMETOOLONG;
            goto fail;
        }

        const char *rest = p;
        while (*rest == '/')
            rest++;
        bool last = !*rest;
        if (name && last) {
            memcpy(name, start, clen);
            name[clen] = '\0';
            break;
        }

        if (cur->type != VNODE_DIR) {
            err = ZK_ENOTDIR;
            goto fail;
        }
        char comp[ZK_NAME_MAX + 1];
        memcpy(comp, start, clen);
        comp[clen] = '\0';

        struct vnode *next;
        err = vfs_permission(cur, MAY_EXEC);
        if (!err)
            err = cur->ops->lookup(cur, comp, &next);
        vnode_put(cur);
        cur = NULL;
        if (err)
            goto out;

        if (next->type == VNODE_SYMLINK && (!last || follow)) {
            if (++links > MAX_LINKS) {
                vnode_put(next);
                err = ZK_ELOOP;
                goto out;
            }
            char target[ZK_PATH_MAX];
            err = read_link(next, target, sizeof target);
            vnode_put(next);
            if (err)
                goto out;
            // rebuild the path: where we are, the link's target, what's left
            bool in_compat = strncmp(done, COMPAT_ROOT "/", sizeof COMPAT_ROOT) == 0 ||
                             strcmp(done, COMPAT_ROOT) == 0;
            if (target[0] == '/')
                snprintf(tmp, ZK_PATH_MAX, "%s%s/%s", in_compat ? COMPAT_ROOT : "", target, rest);
            else
                snprintf(tmp, ZK_PATH_MAX, "%s/%s/%s", done, target, rest);
            err = vfs_normalize("/", tmp, todo);
            if (err)
                goto out;
            goto restart;
        }

        size_t dl = strlen(done);
        if (dl + 1 + clen < ZK_PATH_MAX) {
            done[dl] = '/';
            memcpy(done + dl + 1, comp, clen + 1);
        }
        cur = cross_mount(next);
    }

    if (name) {
        if (!name[0]) {
            err = ZK_EEXIST;    // that was the root
            goto fail;
        }
        if (cur->type != VNODE_DIR) {
            err = ZK_ENOTDIR;
            goto fail;
        }
    }
    *out = cur;
    kfree(buf);
    return 0;

fail:
    if (cur)
        vnode_put(cur);
out:
    kfree(buf);
    return err;
}

int vfs_lookup(const char *path, struct vnode **out)
{
    return walk(path, out, NULL, true);
}

int vfs_lookup_nofollow(const char *path, struct vnode **out)
{
    return walk(path, out, NULL, false);
}

int vfs_symlink(const char *target, const char *path)
{
    struct vnode *dir, *v;
    char name[ZK_NAME_MAX + 1];
    int err = lookup_parent(path, &dir, name);
    if (err)
        return err;
    err = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
    if (!err)
        err = dir->ops->create(dir, name, VNODE_SYMLINK, &v);
    vnode_put(dir);
    if (err)
        return err;
    set_owner(v);
    if (v->ops->setattr)
        v->ops->setattr(v, v->uid, v->gid, 0777);
    int64_t n = v->ops->write(v, target, strlen(target), 0);
    vnode_put(v);
    if (n < 0) {
        vfs_unlink(path);
        return (int)n;
    }
    return 0;
}

int vfs_readlink(const char *path, char *buf, size_t size)
{
    struct vnode *v;
    int err = vfs_lookup_nofollow(path, &v);
    if (err)
        return err;
    err = v->type == VNODE_SYMLINK ? read_link(v, buf, size) : ZK_EINVAL;
    vnode_put(v);
    return err;
}

int vfs_lstat(const char *path, struct zk_stat *st)
{
    struct vnode *v;
    int err = vfs_lookup_nofollow(path, &v);
    if (err)
        return err;
    fill_stat(v, st);
    vnode_put(v);
    return 0;
}

static int lookup_parent(const char *path, struct vnode **dir, char *name)
{
    name[0] = '\0';
    return walk(path, dir, name, true);
}

int vfs_open(const char *path, int flags, struct file **out)
{
    int mode = flags & O_ACCMODE;
    if (mode == O_ACCMODE)
        return ZK_EINVAL;

    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (!err && (flags & O_CREAT) && (flags & O_EXCL)) {
        vnode_put(v);
        return ZK_EEXIST;
    }
    if (!err && (flags & O_DIRECTORY) && v->type != VNODE_DIR) {
        vnode_put(v);
        return ZK_ENOTDIR;
    }
    if (err == ZK_ENOENT && (flags & O_CREAT)) {
        struct vnode *dir;
        char name[ZK_NAME_MAX + 1];
        err = lookup_parent(path, &dir, name);
        if (err)
            return err;
        err = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
        if (!err) {
            err = dir->ops->create(dir, name, VNODE_FILE, &v);
            if (!err)
                set_owner(v);
            // someone else made it first
            else if (err == ZK_EEXIST)
                err = dir->ops->lookup(dir, name, &v);
        }
        vnode_put(dir);
    } else if (!err) {
        int want = mode == O_RDONLY ? MAY_READ : mode == O_WRONLY ? MAY_WRITE : MAY_READ | MAY_WRITE;
        if (flags & O_TRUNC)
            want |= MAY_WRITE;
        err = vfs_permission(v, want);
        if (err)
            vnode_put(v);
    }
    if (err)
        return err;

    if (v->type == VNODE_DIR && mode != O_RDONLY) {
        vnode_put(v);
        return ZK_EISDIR;
    }
    if ((flags & O_TRUNC) && mode != O_RDONLY && v->type == VNODE_FILE && v->ops->truncate) {
        err = v->ops->truncate(v, 0);
        if (err) {
            vnode_put(v);
            return err;
        }
    }

    struct file *f = file_new(v, flags);
    vnode_put(v);
    if (!f)
        return ZK_ENOMEM;
    size_t plen = strlen(path) + 1;
    f->path = kmalloc(plen);
    if (f->path)
        memcpy(f->path, path, plen);
    if (f->v->ops->open) {
        err = f->v->ops->open(f->v, f);
        if (err) {
            // never opened, so no close hook
            vnode_put(f->v);
            kfree(f->path);
            kfree(f);
            return err;
        }
    }
    *out = f;
    return 0;
}

int vfs_mkdir(const char *path)
{
    struct vnode *dir, *v;
    char name[ZK_NAME_MAX + 1];
    int err = lookup_parent(path, &dir, name);
    if (err)
        return err;
    err = vfs_permission(dir, MAY_WRITE | MAY_EXEC);
    if (!err)
        err = dir->ops->create(dir, name, VNODE_DIR, &v);
    vnode_put(dir);
    if (!err) {
        set_owner(v);
        vnode_put(v);
    }
    return err;
}

int vfs_mknod(const char *path, struct vnode *v)
{
    struct vnode *dir;
    char name[ZK_NAME_MAX + 1];
    int err = lookup_parent(path, &dir, name);
    if (err)
        return err;
    err = dir->ops->link ? dir->ops->link(dir, name, v) : ZK_EPERM;
    vnode_put(dir);
    return err;
}

// a hard link, both names on the same kind of filesystem
int vfs_link(const char *from, const char *to, bool follow)
{
    struct vnode *v;
    int err = follow ? vfs_lookup(from, &v) : vfs_lookup_nofollow(from, &v);
    if (err)
        return err;
    struct vnode *odir, *dir;
    char name[ZK_NAME_MAX + 1];
    if (v->type == VNODE_DIR) {
        vnode_put(v);
        return ZK_EPERM;
    }
    err = lookup_parent(from, &odir, name);
    if (err) {
        vnode_put(v);
        return err;
    }
    err = lookup_parent(to, &dir, name);
    if (!err) {
        struct vnode *there;
        if (dir->ops->lookup(dir, name, &there) == 0) {
            vnode_put(there);
            err = ZK_EEXIST;
        } else if (dir->ops != odir->ops || !dir->ops->link) {
            err = ZK_EXDEV;
        } else {
            err = dir->ops->link(dir, name, v);
        }
        vnode_put(dir);
    }
    vnode_put(odir);
    vnode_put(v);
    return err;
}

int vfs_unlink(const char *path)
{
    struct vnode *dir;
    char name[ZK_NAME_MAX + 1];
    int err = lookup_parent(path, &dir, name);
    if (err)
        return err == ZK_EEXIST ? ZK_EPERM : err;
    struct vnode *v;
    err = dir->ops->lookup(dir, name, &v);
    if (!err) {
        if (is_mount_point(v))
            err = ZK_EBUSY;
        else
            err = may_delete(dir, v);
        vnode_put(v);
    }
    if (!err)
        err = dir->ops->unlink(dir, name);
    vnode_put(dir);
    return err;
}

int vfs_rename(const char *from, const char *to)
{
    if (strcmp(from, to) == 0)
        return 0;
    size_t n = strlen(from);
    if (strncmp(to, from, n) == 0 && to[n] == '/')
        return ZK_EINVAL;    // a dir into itself

    struct vnode *odir, *ndir, *v;
    char oname[ZK_NAME_MAX + 1], nname[ZK_NAME_MAX + 1];
    int err = lookup_parent(from, &odir, oname);
    if (err)
        return err == ZK_EEXIST ? ZK_EBUSY : err;
    err = lookup_parent(to, &ndir, nname);
    if (err) {
        vnode_put(odir);
        return err == ZK_EEXIST ? ZK_EBUSY : err;
    }

    err = odir->ops->lookup(odir, oname, &v);
    if (!err) {
        if (is_mount_point(v))
            err = ZK_EBUSY;
        else
            err = may_delete(odir, v);
        vnode_put(v);
    }
    if (!err)
        err = vfs_permission(ndir, MAY_WRITE | MAY_EXEC);
    if (!err && ndir->ops->lookup(ndir, nname, &v) == 0) {
        err = may_delete(ndir, v);
        vnode_put(v);
    }
    if (!err && (odir->ops != ndir->ops || !odir->ops->rename))
        err = ZK_EXDEV;
    if (!err)
        err = odir->ops->rename(odir, oname, ndir, nname);
    vnode_put(odir);
    vnode_put(ndir);
    return err;
}

int vfs_chmod(const char *path, uint32_t mode)
{
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    const struct zk_cred *c = cred_current();
    if (c->euid != 0 && c->euid != v->uid)
        err = ZK_EPERM;
    else if (v->ops->setattr)
        err = v->ops->setattr(v, v->uid, v->gid, mode & 07777);
    // filesystems without permissions just say yes
    vnode_put(v);
    return err;
}

int vfs_chown(const char *path, uint32_t uid, uint32_t gid)
{
    if (cred_current()->euid != 0)
        return ZK_EPERM;
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    if (v->ops->setattr)
        err = v->ops->setattr(v, uid == UINT32_MAX ? v->uid : uid,
                              gid == UINT32_MAX ? v->gid : gid, v->mode & ~ZK_S_ISUID);
    vnode_put(v);
    return err;
}

int vfs_truncate(struct vnode *v, uint64_t size)
{
    if (v->type == VNODE_DIR)
        return ZK_EISDIR;
    if (v->type != VNODE_FILE || !v->ops->truncate)
        return ZK_EINVAL;
    return v->ops->truncate(v, size);
}

void fill_stat(struct vnode *v, struct zk_stat *st)
{
    st->type = v->type;
    st->mode = v->mode;
    st->uid = v->uid;
    st->gid = v->gid;
    st->size = v->size;
    st->ino = v->ino;
    st->mtime = v->mtime;
}

int vfs_stat(const char *path, struct zk_stat *st)
{
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    fill_stat(v, st);
    vnode_put(v);
    return 0;
}

int vfs_read_all(const char *path, void **data, size_t *size, size_t max)
{
    struct vnode *v;
    int err = vfs_lookup(path, &v);
    if (err)
        return err;
    if (v->type == VNODE_DIR) {
        vnode_put(v);
        return ZK_EISDIR;
    }
    if (v->type != VNODE_FILE) {
        vnode_put(v);
        return ZK_EINVAL;
    }
    if (v->size > max) {
        vnode_put(v);
        return ZK_EFBIG;
    }

    size_t len = v->size;
    void *buf = kmalloc(len ? len : 1);
    if (!buf) {
        vnode_put(v);
        return ZK_ENOMEM;
    }
    int64_t n = v->ops->read(v, buf, len, 0);
    vnode_put(v);
    if (n < 0) {
        kfree(buf);
        return (int)n;
    }
    *data = buf;
    *size = (size_t)n;
    return 0;
}

void poll_notify(void)
{
    waitq_wake_all(&poll_wq);
}

bool poll_wait(int64_t ms)
{
    if (ms < 0) {
        waitq_sleep(&poll_wq);
        return true;
    }
    return waitq_sleep_timeout(&poll_wq, (uint64_t)ms);
}

int file_poll(struct file *f)
{
    return f->v->ops->poll ? f->v->ops->poll(f->v) : POLLIN | POLLOUT;
}

int file_ioctl(struct file *f, unsigned long req, uint64_t arg)
{
    return f->v->ops->ioctl ? f->v->ops->ioctl(f->v, f, req, arg) : ZK_EINVAL;
}

struct file *file_new(struct vnode *v, int flags)
{
    struct file *f = kzalloc(sizeof(*f));
    if (!f)
        return NULL;
    vnode_get(v);
    f->v = v;
    f->flags = flags;
    f->refs = 1;
    return f;
}

void file_get(struct file *f)
{
    __atomic_fetch_add(&f->refs, 1, __ATOMIC_RELAXED);
}

void file_put(struct file *f)
{
    if (__atomic_sub_fetch(&f->refs, 1, __ATOMIC_ACQ_REL) == 0) {
        if (f->v->ops->close)
            f->v->ops->close(f->v, f);
        vnode_put(f->v);
        kfree(f->path);
        kfree(f);
    }
}

int64_t file_read(struct file *f, void *buf, size_t len)
{
    if ((f->flags & O_ACCMODE) == O_WRONLY)
        return ZK_EBADF;
    if (f->v->type == VNODE_DIR)
        return ZK_EISDIR;
    if (!f->v->ops->read)
        return ZK_EINVAL;
    int64_t n = f->v->ops->read(f->v, buf, len, f->off);
    if (n > 0)
        f->off += (uint64_t)n;
    return n;
}

int64_t file_write(struct file *f, const void *buf, size_t len)
{
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return ZK_EBADF;
    if (!f->v->ops->write)
        return ZK_EINVAL;
    if (f->flags & O_APPEND)
        f->off = f->v->size;
    int64_t n = f->v->ops->write(f->v, buf, len, f->off);
    if (n > 0)
        f->off += (uint64_t)n;
    return n;
}

int64_t file_seek(struct file *f, int64_t off, int whence)
{
    // block devices have a size and can seek, terminals and pipes can't
    if (f->v->type == VNODE_PIPE || f->v->type == VNODE_SOCK ||
        (f->v->type == VNODE_DEV && !f->v->size))
        return ZK_EINVAL;
    int64_t base;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (int64_t)f->off; break;
    case SEEK_END: base = (int64_t)f->v->size; break;
    default: return ZK_EINVAL;
    }
    if (base + off < 0)
        return ZK_EINVAL;
    f->off = (uint64_t)(base + off);
    return (int64_t)f->off;
}

int file_readdir(struct file *f, struct zk_dirent *out)
{
    if (f->v->type != VNODE_DIR)
        return ZK_ENOTDIR;
    int r = f->v->ops->readdir(f->v, f->off, out);
    if (r > 0)
        f->off++;
    return r;
}

void file_stat(struct file *f, struct zk_stat *st)
{
    fill_stat(f->v, st);
}
