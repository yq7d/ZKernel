#pragma once
// paths are absolute and normalized by the time they get here, see vfs_normalize.
// lookups return referenced vnodes, drop them with vnode_put
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>

enum vnode_type {
    VNODE_FILE = ZK_TYPE_FILE,
    VNODE_DIR  = ZK_TYPE_DIR,
    VNODE_DEV  = ZK_TYPE_DEV,
    VNODE_PIPE = ZK_TYPE_PIPE,
    VNODE_SYMLINK = ZK_TYPE_LINK,
    VNODE_SOCK = ZK_TYPE_SOCK,
};

struct vnode;
struct file;

struct vnode_ops {
    int64_t (*read)(struct vnode *v, void *buf, size_t len, uint64_t off);
    int64_t (*write)(struct vnode *v, const void *buf, size_t len, uint64_t off);
    int (*truncate)(struct vnode *v, uint64_t size);

    // dirs
    int (*lookup)(struct vnode *dir, const char *name, struct vnode **out);
    int (*create)(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out);
    int (*link)(struct vnode *dir, const char *name, struct vnode *v);
    int (*unlink)(struct vnode *dir, const char *name);
    int (*readdir)(struct vnode *dir, size_t index, struct zk_dirent *out);
    // both dirs belong to the same filesystem
    int (*rename)(struct vnode *odir, const char *oname, struct vnode *ndir, const char *nname);

    // last reference gone
    void (*release)(struct vnode *v);

    // changes owner and mode, NULL where the filesystem has no such thing
    int (*setattr)(struct vnode *v, uint32_t uid, uint32_t gid, uint32_t mode);

    // devices: per open file hooks, ioctl, and readiness for poll
    int (*open)(struct vnode *v, struct file *f);
    void (*close)(struct vnode *v, struct file *f);
    int (*ioctl)(struct vnode *v, struct file *f, unsigned long req, uint64_t arg);
    // POLLIN/POLLOUT/POLLHUP bits, called with interrupts off so it can't block
    int (*poll)(struct vnode *v);
    // linux programs' ioctls, numbered the linux way
    int (*lioctl)(struct vnode *v, struct file *f, unsigned long req, uint64_t arg);
    // maps device memory at va (page aligned, len bytes) in the current space
    int (*mmap)(struct vnode *v, struct file *f, uint64_t va, uint64_t len,
                uint64_t off, uint64_t pte_flags);
};

struct vnode {
    enum vnode_type type;
    int refs;
    uint64_t ino;
    uint64_t size;
    int64_t mtime;
    uint32_t uid, gid, mode;
    const struct vnode_ops *ops;
    void *priv;
};

struct mount;

struct fs_type {
    const char *name;
    // fills in m->root and the ops below, source is a device path or ignored
    int (*mount)(struct mount *m, const char *source, const char *opts);
};

struct mount {
    char path[ZK_PATH_MAX];
    char source[ZK_PATH_MAX];
    char opts[64];
    const struct fs_type *type;
    struct vnode *root;
    struct vnode *covered;    // the dir it sits on, NULL for /
    void *sb;
    int (*statfs)(struct mount *m, struct zk_statfs *st);
    int (*sync)(struct mount *m);
    int (*unmount)(struct mount *m);    // ZK_EBUSY while files are open
    struct mount *next;
};

struct file {
    struct vnode *v;
    uint64_t off;
    int flags;
    int refs;
    char *path;    // what it was opened as, NULL for pipes and the like
};

void vfs_init(void);
uint64_t vfs_next_ino(void);

void vnode_get(struct vnode *v);
void vnode_put(struct vnode *v);

// joins path onto cwd and resolves . and .. into out (ZK_PATH_MAX bytes)
int vfs_normalize(const char *cwd, const char *path, char *out);

int vfs_lookup(const char *path, struct vnode **out);
int vfs_lookup_nofollow(const char *path, struct vnode **out);
int vfs_symlink(const char *target, const char *path);
int vfs_readlink(const char *path, char *buf, size_t size);
int vfs_lstat(const char *path, struct zk_stat *st);
void fill_stat(struct vnode *v, struct zk_stat *st);
int vfs_open(const char *path, int flags, struct file **out);
int vfs_mkdir(const char *path);
int vfs_unlink(const char *path);
int vfs_stat(const char *path, struct zk_stat *st);
// puts an existing vnode (a device) into the tree
int vfs_mknod(const char *path, struct vnode *v);
int vfs_rename(const char *from, const char *to);
int vfs_truncate(struct vnode *v, uint64_t size);
int vfs_mount(const char *source, const char *target, const char *fstype, const char *opts);
int vfs_chmod(const char *path, uint32_t mode);
int vfs_chown(const char *path, uint32_t uid, uint32_t gid);

#define MAY_EXEC  1
#define MAY_WRITE 2
#define MAY_READ  4
// checks the caller's credentials against v
int vfs_permission(struct vnode *v, int want);
// "uid=1000,mode=755" style mount options. false if key isn't there
bool mount_opt(const char *opts, const char *key, uint32_t *val, int base);
int vfs_umount(const char *target);
int vfs_statfs(const char *path, struct zk_statfs *st);
void vfs_sync(void);
void vfs_register(const struct fs_type *type);
// calls fn for every mount, under the mount lock
void vfs_each_mount(void (*fn)(struct mount *m, void *ctx), void *ctx);
// reads a whole file into a new kmalloc buffer
int vfs_read_all(const char *path, void **data, size_t *size, size_t max);

struct file *file_new(struct vnode *v, int flags);
void file_get(struct file *f);
void file_put(struct file *f);
int64_t file_read(struct file *f, void *buf, size_t len);
int64_t file_write(struct file *f, const void *buf, size_t len);
int64_t file_seek(struct file *f, int64_t off, int whence);
int file_readdir(struct file *f, struct zk_dirent *out);
void file_stat(struct file *f, struct zk_stat *st);

// ramfs.c
extern const struct fs_type ramfs_type;
size_t ramfs_bytes_used(void);

// procfs.c
extern const struct fs_type procfs_type;

// fat.c
extern const struct fs_type fat_type;

// ext2.c, along with bind mounts
extern const struct fs_type ext2_type;
extern const struct fs_type bind_type;

// initrd.c, unpacks a ustar archive into the tree. returns files created
int initrd_unpack(const void *data, size_t size);

// pipe.c
int pipe_create(struct file **rd, struct file **wr);

// pty.c, rd is the master side
int pty_create(struct file **master, struct file **slave);

// wakes poll() sleepers, call whenever something may have become readable
void poll_notify(void);
// interrupts must be off. true if woken, false on timeout. ms < 0 waits forever
bool poll_wait(int64_t ms);
int file_poll(struct file *f);
int file_ioctl(struct file *f, unsigned long req, uint64_t arg);

// dev.c
void dev_init(void);
// linux major:minor of a device inode, 0 if unknown
uint32_t dev_rdev(uint64_t ino);
