// linux x86_64 syscalls on top of the zos ones. enough for static glibc
// programs and for musl's dynamic loader, so alpine packages run. single
// threaded: clone with CLONE_VM is only accepted the way vfork uses it
#include "proc/linux.h"
#include "proc/proc.h"
#include "proc/futex.h"
#include "proc/sys.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/interrupts.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/random.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "net/net.h"
#include "sched/sched.h"

#define AT_FDCWD            (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR        0x200
#define AT_EMPTY_PATH       0x1000

// linux errno values, indexed by -ZK_E*
static const int16_t errno_map[] = {
    [0] = 0,
    [-ZK_EFAULT] = 14, [-ZK_EINVAL] = 22, [-ZK_ENOMEM] = 12, [-ZK_ENOSYS] = 38,
    [-ZK_ENOENT] = 2, [-ZK_EBADF] = 9, [-ZK_EEXIST] = 17, [-ZK_ENOTDIR] = 20,
    [-ZK_EISDIR] = 21, [-ZK_ENOTEMPTY] = 39, [-ZK_EMFILE] = 24, [-ZK_ECHILD] = 10,
    [-ZK_ENOEXEC] = 8, [-ZK_ENAMETOOLONG] = 36, [-ZK_EPIPE] = 32, [-ZK_EINTR] = 4,
    [-ZK_ESRCH] = 3, [-ZK_EFBIG] = 27, [-ZK_EPERM] = 1, [-ZK_E2BIG] = 7,
    [-ZK_EBUSY] = 16, [-ZK_EXDEV] = 18, [-ZK_ENOSPC] = 28, [-ZK_EIO] = 5,
    [-ZK_EROFS] = 30, [-ZK_ENODEV] = 19, [-ZK_EACCES] = 13, [-ZK_ELOOP] = 40,
    [-ZK_ENOTTY] = 25, [-ZK_EAGAIN] = 11, [-ZK_ENOTSOCK] = 88, [-ZK_ECONNREFUSED] = 111,
    [-ZK_ETIMEDOUT] = 110, [-ZK_ENETUNREACH] = 101, [-ZK_EADDRINUSE] = 98,
    [-ZK_ENOTCONN] = 107, [-ZK_ECONNRESET] = 104, [-ZK_EHOSTUNREACH] = 113,
    [-ZK_EINPROGRESS] = 115,
};

#define LENOSYS (-38)
#define LENOTTY (-25)
#define LEINVAL (-22)

static int64_t lerr(int64_t r)
{
    if (r >= 0)
        return r;
    if (-r <= ZK_ERRNO_MAX)
        return -errno_map[-r];
    return LEINVAL;
}

// paths

static bool excluded(const char *path)
{
    static const char *const real[] = { "/dev", "/proc", "/tmp", "/home", "/mnt", "/run", LINUX_ROOT };
    for (size_t i = 0; i < sizeof real / sizeof real[0]; i++) {
        size_t n = strlen(real[i]);
        if (strncmp(path, real[i], n) == 0 && (path[n] == '/' || !path[n]))
            return true;
    }
    return false;
}

void linux_resolve(const char *path, char *out, bool creating)
{
    strlcpy(out, path, ZK_PATH_MAX);
    if (excluded(path) || strcmp(path, "/") == 0)
        return;
    char alt[ZK_PATH_MAX];
    if (snprintf(alt, sizeof alt, LINUX_ROOT "%s", path) >= (int)sizeof alt)
        return;
    struct vnode *v;
    if (vfs_lookup_nofollow(alt, &v) == 0) {
        vnode_put(v);
        strlcpy(out, alt, ZK_PATH_MAX);
        return;
    }
    if (!creating)
        return;
    // a new file goes on the linux side whenever its directory is there
    char *slash = strrchr(alt, '/');
    if (slash && slash > alt + sizeof LINUX_ROOT - 1) {
        *slash = '\0';
        if (vfs_lookup(alt, &v) == 0) {
            vnode_put(v);
            *slash = '/';
            strlcpy(out, alt, ZK_PATH_MAX);
        }
    }
}

static struct proc *me(void)
{
    return proc_current();
}

static struct file *fd_file(int fd)
{
    return proc_fd_get(me(), fd);
}

// a path relative to dirfd, resolved for linux
static int at_path(int dirfd, uint64_t upath, char *out, bool creating)
{
    char raw[ZK_PATH_MAX], abs[ZK_PATH_MAX];
    int err = get_string(upath, raw, sizeof raw);
    if (err)
        return err;
    const char *base = me()->cwd;
    if (raw[0] != '/' && dirfd != AT_FDCWD) {
        struct file *f = fd_file(dirfd);
        if (!f)
            return ZK_EBADF;
        if (!f->path || f->v->type != VNODE_DIR)
            return ZK_ENOTDIR;
        base = f->path;
    }
    err = vfs_normalize(base, raw, abs);
    if (err)
        return err;
    linux_resolve(abs, out, creating);
    return 0;
}

// structs

struct linux_stat {
    uint64_t dev, ino, nlink;
    uint32_t mode, uid, gid, pad0;
    uint64_t rdev;
    int64_t size, blksize, blocks;
    uint64_t atime, atime_ns, mtime, mtime_ns, ctime, ctime_ns;
    int64_t unused[3];
};

static uint32_t type_bits(uint32_t type)
{
    switch (type) {
    case ZK_TYPE_DIR:  return 0040000;
    case ZK_TYPE_DEV:  return 0020000;
    case ZK_TYPE_PIPE: return 0010000;
    case ZK_TYPE_LINK: return 0120000;
    case ZK_TYPE_SOCK: return 0140000;
    default:           return 0100000;
    }
}

static int64_t put_stat(const struct zk_stat *st, uint64_t ubuf)
{
    struct linux_stat l;
    memset(&l, 0, sizeof l);
    l.dev = 1;
    l.ino = st->ino;
    l.nlink = st->type == ZK_TYPE_DIR ? 2 : 1;
    l.mode = type_bits(st->type) | (st->mode & 07777);
    l.uid = st->uid;
    l.gid = st->gid;
    l.rdev = st->type == ZK_TYPE_DEV ? 0x0501 : 0;
    l.size = (int64_t)st->size;
    l.blksize = 4096;
    l.blocks = (int64_t)((st->size + 511) / 512);
    l.atime = l.mtime = l.ctime = (uint64_t)st->mtime;
    return copy_out(ubuf, &l, sizeof l);
}

static int64_t l_stat_at(int dirfd, uint64_t upath, uint64_t ubuf, int flags)
{
    struct zk_stat st;
    char path[ZK_PATH_MAX];
    if (flags & AT_EMPTY_PATH) {
        char c;
        if (copy_in(&c, upath, 1) == 0 && c == '\0') {
            struct file *f = dirfd == AT_FDCWD ? NULL : fd_file(dirfd);
            if (!f)
                return ZK_EBADF;
            file_stat(f, &st);
            return put_stat(&st, ubuf);
        }
    }
    int err = at_path(dirfd, upath, path, false);
    if (!err)
        err = (flags & AT_SYMLINK_NOFOLLOW) ? vfs_lstat(path, &st) : vfs_stat(path, &st);
    return err ? err : put_stat(&st, ubuf);
}

static int64_t l_fstat(int fd, uint64_t ubuf)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    struct zk_stat st;
    file_stat(f, &st);
    return put_stat(&st, ubuf);
}

// files

static int set_fd(struct proc *p, int fd, struct file *f)
{
    if (fd < 0 || fd >= PROC_MAX_FDS)
        return ZK_EBADF;
    if (p->fds[fd])
        file_put(p->fds[fd]);
    p->fds[fd] = f;
    p->cloexec &= ~(1U << fd);
    return fd;
}

static int alloc_fd_from(struct proc *p, int min, struct file *f)
{
    for (int i = min; i < PROC_MAX_FDS; i++) {
        if (!p->fds[i]) {
            p->fds[i] = f;
            p->cloexec &= ~(1U << i);
            return i;
        }
    }
    return ZK_EMFILE;
}

static int64_t l_openat(int dirfd, uint64_t upath, int flags, uint32_t mode)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, flags & O_CREAT);
    if (err)
        return err;
    struct zk_stat st;
    bool existed = vfs_stat(path, &st) == 0;
    struct file *f;
    err = vfs_open(path, flags & (O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND | O_DIRECTORY), &f);
    if (err)
        return err;
    if (!existed && (flags & O_CREAT))
        vfs_chmod(path, mode & 07777 & ~022);
    int fd = proc_fd_alloc(me(), f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (flags & O_CLOEXEC)
        me()->cloexec |= 1U << fd;
    return fd;
}

static int64_t l_close(int fd)
{
    struct proc *p = me();
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return ZK_EBADF;
    p->fds[fd] = NULL;
    p->cloexec &= ~(1U << fd);
    file_put(f);
    return 0;
}

static int64_t l_rw(int fd, uint64_t buf, size_t len, bool write, int64_t off)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    if (!user_ok(buf, len, !write))
        return ZK_EFAULT;
    if (f->v->type == VNODE_DIR && !write)
        return ZK_EISDIR;
    uint64_t saved = f->off;
    if (off >= 0)
        f->off = (uint64_t)off;
    int64_t n = write ? file_write(f, (const void *)buf, len) : file_read(f, (void *)buf, len);
    if (off >= 0)
        f->off = saved;
    return n;
}

struct iovec {
    uint64_t base, len;
};

static int64_t l_rwv(int fd, uint64_t uiov, int cnt, bool write, int64_t off)
{
    if (cnt < 0 || cnt > 1024)
        return ZK_EINVAL;
    int64_t total = 0;
    for (int i = 0; i < cnt; i++) {
        struct iovec v;
        if (copy_in(&v, uiov + (uint64_t)i * sizeof v, sizeof v))
            return ZK_EFAULT;
        if (!v.len)
            continue;
        int64_t n = l_rw(fd, v.base, v.len, write, off >= 0 ? off + total : -1);
        if (n < 0)
            return total ? total : n;
        total += n;
        if ((uint64_t)n < v.len)
            break;
    }
    return total;
}

struct linux_dirent64 {
    uint64_t ino;
    int64_t off;
    uint16_t reclen;
    uint8_t type;
    char name[];
} __attribute__((packed));

static int64_t l_getdents64(int fd, uint64_t ubuf, size_t count)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    if (f->v->type != VNODE_DIR)
        return ZK_ENOTDIR;
    if (!user_ok(ubuf, count, true))
        return ZK_EFAULT;
    uint8_t *out = (uint8_t *)ubuf;
    size_t used = 0;
    // . and .. come first, like everywhere else
    while (f->off < 2) {
        const char *name = f->off == 0 ? "." : "..";
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + strlen(name) + 1, 8);
        if (used + reclen > count)
            return used ? (int64_t)used : ZK_EINVAL;
        struct linux_dirent64 *d = (void *)(out + used);
        d->ino = f->v->ino + f->off;
        d->off = (int64_t)f->off + 1;
        d->reclen = (uint16_t)reclen;
        d->type = 4;
        strlcpy(d->name, name, reclen - sizeof *d);
        used += reclen;
        f->off++;
    }
    for (;;) {
        struct zk_dirent e;
        uint64_t idx = f->off - 2;
        int r = f->v->ops->readdir(f->v, idx, &e);
        if (r <= 0)
            break;
        size_t reclen = ALIGN_UP(sizeof(struct linux_dirent64) + strlen(e.name) + 1, 8);
        if (used + reclen > count) {
            if (!used)
                return ZK_EINVAL;
            break;
        }
        struct linux_dirent64 *d = (void *)(out + used);
        d->ino = idx + 3;
        d->off = (int64_t)f->off + 1;
        d->reclen = (uint16_t)reclen;
        static const uint8_t dt[] = { 0, 8, 4, 2, 1, 10 };
        d->type = e.type < sizeof dt ? dt[e.type] : 0;
        strlcpy(d->name, e.name, reclen - sizeof *d);
        used += reclen;
        f->off++;
    }
    return (int64_t)used;
}

// terminals

struct termios {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
};

struct termios2 {
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t line;
    uint8_t cc[19];
    uint32_t ispeed, ospeed;
};

#define L_ISIG   0000001
#define L_ICANON 0000002
#define L_ECHO   0000010

static int64_t l_ioctl(int fd, unsigned long req, uint64_t arg)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    struct tty *t = tty_of(f->v);
    switch (req) {
    case 0x5401:        // TCGETS
    case 0x802C542A: {  // TCGETS2, termios plus two speeds
        if (!t)
            return ZK_ENOTTY;
        struct termios2 tio;
        memset(&tio, 0, sizeof tio);
        tio.iflag = 0002400;    // ICRNL | IXON
        tio.oflag = 0000005;    // OPOST | ONLCR
        tio.cflag = 0000277;    // CS8 | CREAD | B38400
        tio.lflag = 0000060;    // ECHOE | ECHOK
        if (t->mode & ZK_TTY_CANON)
            tio.lflag |= L_ICANON;
        if (t->mode & ZK_TTY_ECHO)
            tio.lflag |= L_ECHO;
        if (t->mode & ZK_TTY_ISIG)
            tio.lflag |= L_ISIG;
        static const uint8_t cc[] = { 3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23, 22 };
        memcpy(tio.cc, cc, sizeof cc);
        tio.ispeed = tio.ospeed = 38400;
        return copy_out(arg, &tio, req == 0x5401 ? sizeof(struct termios) : sizeof tio);
    }
    case 0x5402:        // TCSETS, TCSETSW, TCSETSF
    case 0x5403:
    case 0x5404:
    case 0x402C542B:    // and the termios2 ones
    case 0x402C542C:
    case 0x402C542D: {
        if (!t)
            return ZK_ENOTTY;
        struct termios tio;
        if (copy_in(&tio, arg, sizeof tio))
            return ZK_EFAULT;
        uint32_t mode = 0;
        if (tio.lflag & L_ICANON)
            mode |= ZK_TTY_CANON;
        if (tio.lflag & L_ECHO)
            mode |= ZK_TTY_ECHO;
        if (tio.lflag & L_ISIG)
            mode |= ZK_TTY_ISIG;
        t->mode = mode;
        return 0;
    }
    case 0x5413:    // TIOCGWINSZ
        if (!t)
            return ZK_ENOTTY;
        return copy_out(arg, &t->ws, sizeof t->ws);
    case 0x5414:    // TIOCSWINSZ
        if (!t)
            return ZK_ENOTTY;
        return copy_in(&t->ws, arg, sizeof t->ws);
    case 0x540F: {  // TIOCGPGRP
        if (!t)
            return ZK_ENOTTY;
        int32_t pg = (int32_t)(t->fg ? t->fg : me()->pgid);
        return copy_out(arg, &pg, sizeof pg);
    }
    case 0x5410: {  // TIOCSPGRP
        if (!t)
            return ZK_ENOTTY;
        int32_t pg;
        if (copy_in(&pg, arg, sizeof pg))
            return ZK_EFAULT;
        t->fg = (uint64_t)pg;
        return 0;
    }
    case 0x5421: {  // FIONBIO
        int32_t on;
        if (copy_in(&on, arg, 4))
            return ZK_EFAULT;
        sock_set_nonblock(f->v, on != 0);
        return 0;
    }
    case 0x540E:    // TIOCSCTTY
    case 0x5451:    // FIOCLEX
        return t ? 0 : ZK_ENOTTY;
    case 0x541B: {  // FIONREAD
        int32_t n = t ? (int32_t)t->in_count : 0;
        return copy_out(arg, &n, sizeof n);
    }
    }
    return t ? ZK_EINVAL : ZK_ENOTTY;
}

static int64_t l_fcntl(int fd, int cmd, uint64_t arg)
{
    struct proc *p = me();
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return ZK_EBADF;
    switch (cmd) {
    case 0:         // F_DUPFD
    case 1030: {    // F_DUPFD_CLOEXEC
        int n = alloc_fd_from(p, (int)arg, f);
        if (n < 0)
            return n;
        file_get(f);
        if (cmd == 1030)
            p->cloexec |= 1U << n;
        return n;
    }
    case 1:         // F_GETFD
        return (p->cloexec >> fd) & 1;
    case 2:         // F_SETFD
        if (arg & 1)
            p->cloexec |= 1U << fd;
        else
            p->cloexec &= ~(1U << fd);
        return 0;
    case 3:         // F_GETFL
        return f->flags & (O_ACCMODE | O_APPEND);
    case 4:         // F_SETFL
        f->flags = (f->flags & ~O_APPEND) | ((int)arg & O_APPEND);
        sock_set_nonblock(f->v, arg & O_NONBLOCK);
        return 0;
    default:        // locks and the rest: nothing else runs at the same time anyway
        return 0;
    }
}

// memory

#define PROT_WRITE 2
#define PROT_EXEC  4
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_FIXED_NOREPLACE 0x100000

static uint64_t prot_flags(int prot)
{
    uint64_t f = 0;
    if (prot & PROT_WRITE)
        f |= PTE_WRITE;
    if (!(prot & PROT_EXEC))
        f |= PTE_NX;
    return f;
}

static void unmap_range(struct address_space *as, uint64_t start, uint64_t len)
{
    for (uint64_t v = start; v < start + len; v += PAGE_SIZE)
        vmm_free_user(as, v);
}

static int64_t l_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    struct address_space *as = thread_current()->as;
    if (!len || (off & (PAGE_SIZE - 1)))
        return ZK_EINVAL;
    len = ALIGN_UP(len, PAGE_SIZE);
    if (len > (64UL << 30))
        return ZK_ENOMEM;

    struct file *f = NULL;
    if (!(flags & MAP_ANONYMOUS)) {
        f = fd_file(fd);
        if (!f)
            return ZK_EBADF;
        if (f->v->type != VNODE_FILE)
            return ZK_ENODEV;
    }

    uint64_t start;
    if (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) {
        if (addr & (PAGE_SIZE - 1) || addr < 0x10000 || addr + len > USER_LIMIT)
            return ZK_EINVAL;
        start = addr;
        unmap_range(as, start, len);
    } else {
        if (as->mmap_next - len < as->brk + (1UL << 30))
            return ZK_ENOMEM;
        start = as->mmap_next - len;
        as->mmap_next = start;
    }

    // reservations get no pages, mprotect fills them in later
    if (prot == 0 && !f)
        return (int64_t)start;

    uint64_t pf = prot_flags(prot);
    for (uint64_t v = start; v < start + len; v += PAGE_SIZE) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p || !vmm_map_user(as, v, p, pf)) {
            if (p)
                pmm_free(p);
            unmap_range(as, start, v - start);
            return ZK_ENOMEM;
        }
        if (f) {
            // a private copy of the file, there's no page cache to share
            uint64_t pos = off + (v - start);
            if (pos < f->v->size) {
                size_t n = f->v->size - pos < PAGE_SIZE ? f->v->size - pos : PAGE_SIZE;
                int64_t got = f->v->ops->read(f->v, phys_to_virt(p), n, pos);
                if (got < 0) {
                    unmap_range(as, start, v - start + PAGE_SIZE);
                    return got;
                }
            }
        }
    }
    (void)MAP_SHARED;
    return (int64_t)start;
}

static int64_t l_munmap(uint64_t addr, uint64_t len)
{
    if (addr & (PAGE_SIZE - 1) || !len)
        return ZK_EINVAL;
    unmap_range(thread_current()->as, addr, ALIGN_UP(len, PAGE_SIZE));
    return 0;
}

static int64_t l_mprotect(uint64_t addr, uint64_t len, int prot)
{
    struct address_space *as = thread_current()->as;
    if (addr & (PAGE_SIZE - 1))
        return ZK_EINVAL;
    uint64_t pf = prot_flags(prot);
    for (uint64_t v = addr; v < addr + ALIGN_UP(len, PAGE_SIZE); v += PAGE_SIZE) {
        if (vmm_protect_user(as, v, pf))
            continue;
        if (prot == 0)
            continue;
        // a reservation coming to life
        uint64_t p = pmm_alloc_zeroed();
        if (!p)
            return ZK_ENOMEM;
        if (!vmm_map_user(as, v, p, pf)) {
            pmm_free(p);
            return ZK_ENOMEM;
        }
    }
    return 0;
}

static int64_t l_brk(uint64_t want)
{
    struct address_space *as = thread_current()->as;
    if (want < as->brk_start || want > as->brk_start + (1UL << 32))
        return (int64_t)as->brk;
    uint64_t old = ALIGN_UP(as->brk, PAGE_SIZE), new = ALIGN_UP(want, PAGE_SIZE);
    for (uint64_t v = old; v < new; v += PAGE_SIZE) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p || !vmm_map_user(as, v, p, PTE_WRITE | PTE_NX)) {
            if (p)
                pmm_free(p);
            unmap_range(as, old, v - old);
            return (int64_t)as->brk;
        }
    }
    if (new < old)
        unmap_range(as, new, old - new);
    as->brk = want;
    return (int64_t)want;
}

// processes

static int64_t l_wait4(int64_t pid, uint64_t ustatus, int options, uint64_t urusage)
{
    if (ustatus && !user_ok(ustatus, 4, true))
        return ZK_EFAULT;
    if (urusage) {
        uint8_t zero[144] = { 0 };
        if (copy_out(urusage, zero, sizeof zero))
            return ZK_EFAULT;
    }
    int code = 0;
    int64_t r = proc_wait(pid > 0 ? pid : -1, &code, options & 1 ? WNOHANG : 0);
    if (r > 0 && ustatus) {
        int sig = PROC_STATUS_SIG(code);
        int status = sig ? sig | (PROC_STATUS_CORE(code) ? 0x80 : 0)
                         : (code & 0xff) << 8;
        *(int32_t *)ustatus = status;
    }
    return r;
}

static int64_t l_clone(struct interrupt_frame *f, uint64_t flags, uint64_t stack,
                       uint64_t ptid, uint64_t ctid, uint64_t tls)
{
    if (flags & CLONE_THREAD) {
        // threads share everything, anything else isn't a thread here
        uint64_t need = CLONE_VM | CLONE_SIGHAND | CLONE_FILES | CLONE_FS;
        if ((flags & need) != need)
            return ZK_EINVAL;
        return proc_clone_thread(f, flags, stack, ptid, ctid, tls);
    }
    if ((flags & CLONE_VM) && !(flags & CLONE_VFORK))
        return ZK_ENOSYS;
    // vfork style children get a copy instead of the shared memory, which
    // is fine since they only exec or exit
    return proc_fork(f, stack, (flags & CLONE_CHILD_SETTID) ? ctid : 0);
}

#define FUTEX_WAIT           0
#define FUTEX_WAKE           1
#define FUTEX_REQUEUE        3
#define FUTEX_CMP_REQUEUE    4
#define FUTEX_WAIT_BITSET    9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_CLOCK_REALTIME 256

static int64_t l_futex(uint64_t uaddr, int op, uint32_t val, uint64_t utime,
                       uint64_t uaddr2, uint32_t val3)
{
    int cmd = op & 0x7f;
    struct address_space *as = thread_current()->as;
    switch (cmd) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        int64_t ms = -1;
        if (utime) {
            int64_t ts[2];
            if (copy_in(ts, utime, sizeof ts))
                return ZK_EFAULT;
            if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000)
                return ZK_EINVAL;
            int64_t t = ts[0] * 1000 + (ts[1] + 999999) / 1000000;
            if (cmd == FUTEX_WAIT_BITSET) {
                // absolute, on the monotonic clock unless told otherwise
                uint64_t up = timer_uptime_ms();
                int64_t now = (op & FUTEX_CLOCK_REALTIME)
                            ? rtc_now() * 1000 + (int64_t)(up % 1000) : (int64_t)up;
                t -= now;
                if (t < 0)
                    t = 0;
            }
            ms = t;
        }
        int64_t r = futex_wait(uaddr, val, ms,
                               cmd == FUTEX_WAIT ? FUTEX_BITSET_ANY : val3);
        return r;
    }
    case FUTEX_WAKE:
        return futex_wake(as, uaddr, (int)val, FUTEX_BITSET_ANY);
    case FUTEX_WAKE_BITSET:
        return val3 ? futex_wake(as, uaddr, (int)val, val3) : ZK_EINVAL;
    case FUTEX_CMP_REQUEUE: {
        uint32_t cur;
        if (copy_in(&cur, uaddr, 4))
            return ZK_EFAULT;
        if (cur != val3)
            return ZK_EAGAIN;
    }
    // fall through
    case FUTEX_REQUEUE:
        // the timeout slot carries how many to move
        return futex_requeue(uaddr, (int)val, uaddr2, (int)(uint32_t)utime);
    }
    return ZK_ENOSYS;
}

static int64_t l_tkill(int64_t tgid, int64_t tid, int sig)
{
    if (tid <= 0 || sig < 0 || sig > NSIG)
        return ZK_EINVAL;
    uint64_t irq = irq_save();
    struct thread *t = proc_find_thread((uint64_t)tid);
    if (!t || !t->proc || (tgid > 0 && t->proc->pid != (uint64_t)tgid)) {
        irq_restore(irq);
        return ZK_ESRCH;
    }
    const struct zk_cred *c = cred_current();
    const struct zk_cred *tc = &t->proc->cred;
    if (c->euid != 0 && c->uid != tc->uid && c->euid != tc->uid &&
        c->uid != tc->suid && c->euid != tc->suid) {
        irq_restore(irq);
        return ZK_EPERM;
    }
    if (sig) {
        struct ksiginfo si = { .code = SI_TKILL, .pid = (int)me()->pid, .uid = c->uid };
        signal_send_thread(t, sig, &si);
    }
    irq_restore(irq);
    return 0;
}

static int64_t l_execve(uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    struct exec_args *a;
    int err = get_exec_args(upath, uargv, uenvp, &a);
    if (err)
        return err;
    char path[ZK_PATH_MAX];
    linux_resolve(a->path, path, false);
    err = proc_exec(path, a->argv.ptrs, a->envp.ptrs, a);
    kfree(a);
    return err;
}

static int64_t l_kill(int64_t pid, int sig, int code)
{
    return proc_signal(pid, sig, code);
}

// signals

static int64_t l_sigaction(int sig, uint64_t uact, uint64_t uold, uint64_t size)
{
    if (size != 8 || sig < 1 || sig > NSIG)
        return ZK_EINVAL;
    struct proc *p = me();
    struct ksigaction act;
    if (uact) {
        if (sig == SIGKILL || sig == SIGSTOP)
            return ZK_EINVAL;
        if (copy_in(&act, uact, sizeof act))
            return ZK_EFAULT;
        act.mask &= ~SIG_UNBLOCKABLE;
    }
    if (uold && copy_out(uold, &p->sigact[sig - 1], sizeof act))
        return ZK_EFAULT;
    if (uact) {
        uint64_t flags = irq_save();
        p->sigact[sig - 1] = act;
        // a pending signal that's now ignored goes away
        if (act.handler == SIG_IGN || (act.handler == SIG_DFL && signal_default_ignores(sig))) {
            p->sig_pending &= ~SIGBIT(sig);
            for (struct thread *t = p->threads; t; t = t->tg_next)
                t->sig_pending &= ~SIGBIT(sig);
        }
        irq_restore(flags);
    }
    return 0;
}

static int64_t l_sigprocmask(int how, uint64_t uset, uint64_t uold, uint64_t size)
{
    if (size != 8)
        return ZK_EINVAL;
    struct thread *p = thread_current();
    uint64_t old = p->sig_blocked, set = 0;
    if (uset && copy_in(&set, uset, 8))
        return ZK_EFAULT;
    if (uset) {
        uint64_t n;
        switch (how) {
        case 0:  n = old | set; break;     // SIG_BLOCK
        case 1:  n = old & ~set; break;    // SIG_UNBLOCK
        case 2:  n = set; break;           // SIG_SETMASK
        default: return ZK_EINVAL;
        }
        uint64_t flags = irq_save();
        p->sig_blocked = n & ~SIG_UNBLOCKABLE;
        irq_restore(flags);
    }
    return uold && copy_out(uold, &old, 8) ? ZK_EFAULT : 0;
}

static struct waitq never = WAITQ_INIT;

// sleeps until a signal shows up
static int64_t wait_signal(void)
{
    uint64_t flags = irq_save();
    while (!proc_killed())
        waitq_sleep(&never);
    irq_restore(flags);
    return ZK_EINTR;
}

static int64_t l_sigsuspend(uint64_t umask, uint64_t size)
{
    uint64_t mask;
    if (size != 8)
        return ZK_EINVAL;
    if (copy_in(&mask, umask, 8))
        return ZK_EFAULT;
    struct thread *p = thread_current();
    uint64_t flags = irq_save();
    p->sig_saved = p->sig_blocked;
    p->sig_restore = true;
    p->sig_blocked = mask & ~SIG_UNBLOCKABLE;
    irq_restore(flags);
    return wait_signal();
}

static int64_t l_sigpending(uint64_t uset, uint64_t size)
{
    if (size != 8)
        return ZK_EINVAL;
    struct thread *t = thread_current();
    uint64_t set = (t->sig_pending | me()->sig_pending) & t->sig_blocked;
    return copy_out(uset, &set, 8) ? ZK_EFAULT : 0;
}

struct l_stack_t {
    uint64_t sp;
    int32_t flags, pad;
    uint64_t size;
};

static int64_t l_sigaltstack(uint64_t uss, uint64_t uold, uint64_t rsp)
{
    struct thread *p = thread_current();
    bool on = p->altstack_size && rsp > p->altstack_sp &&
              rsp <= p->altstack_sp + p->altstack_size;
    if (uold) {
        struct l_stack_t o = {
            .sp = p->altstack_sp, .size = p->altstack_size,
            .flags = on ? SS_ONSTACK : p->altstack_flags & SS_DISABLE,
        };
        if (copy_out(uold, &o, sizeof o))
            return ZK_EFAULT;
    }
    if (uss) {
        struct l_stack_t n;
        if (copy_in(&n, uss, sizeof n))
            return ZK_EFAULT;
        if (on)
            return ZK_EPERM;
        if (n.flags & ~(SS_DISABLE | (int)(1U << 31)))
            return ZK_EINVAL;
        if (n.flags & SS_DISABLE) {
            p->altstack_sp = p->altstack_size = 0;
            p->altstack_flags = SS_DISABLE;
        } else {
            if (n.size < 2048)
                return -1000 - 12;    // ENOMEM, what linux says for MINSIGSTKSZ
            p->altstack_sp = n.sp;
            p->altstack_size = n.size;
            p->altstack_flags = 0;
        }
    }
    return 0;
}

// time and system info

struct timespec {
    int64_t sec, nsec;
};

static int64_t l_clock_gettime(int clk, uint64_t uts)
{
    struct timespec ts;
    uint64_t ms = timer_uptime_ms();
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) {
        ts.sec = rtc_now();
        ts.nsec = (int64_t)(ms % 1000) * 1000000;
    } else if (clk == 2 || clk == 3) {
        uint64_t t = thread_current()->cpu_ticks * 1000 / TIMER_HZ;
        ts.sec = (int64_t)(t / 1000);
        ts.nsec = (int64_t)(t % 1000) * 1000000;
    } else {
        ts.sec = (int64_t)(ms / 1000);
        ts.nsec = (int64_t)(ms % 1000) * 1000000;
    }
    return copy_out(uts, &ts, sizeof ts);
}

static int64_t l_nanosleep(uint64_t ureq)
{
    struct timespec ts;
    if (copy_in(&ts, ureq, sizeof ts))
        return ZK_EFAULT;
    if (ts.sec < 0 || ts.nsec < 0)
        return ZK_EINVAL;
    uint64_t ms = (uint64_t)ts.sec * 1000 + (uint64_t)(ts.nsec + 999999) / 1000000;
    thread_sleep_ms(ms);
    return proc_killed() ? ZK_EINTR : 0;
}

struct utsname {
    char sysname[65], nodename[65], release[65], version[65], machine[65], domain[65];
};

static int64_t l_uname(uint64_t ubuf)
{
    struct utsname u;
    memset(&u, 0, sizeof u);
    strlcpy(u.sysname, "Linux", sizeof u.sysname);
    strlcpy(u.release, "6.6.0-zos", sizeof u.release);
    strlcpy(u.version, "#1 ZOS", sizeof u.version);
    strlcpy(u.machine, "x86_64", sizeof u.machine);
    strlcpy(u.domain, "(none)", sizeof u.domain);
    strlcpy(u.nodename, "zkernel", sizeof u.nodename);
    void *data;
    size_t size;
    if (vfs_read_all("/etc/hostname", &data, &size, 64) == 0) {
        size_t n = 0;
        while (n < size && n < 64 && ((char *)data)[n] != '\n')
            n++;
        if (n) {
            memcpy(u.nodename, data, n);
            u.nodename[n] = '\0';
        }
        kfree(data);
    }
    return copy_out(ubuf, &u, sizeof u);
}

struct sysinfo {
    int64_t uptime;
    uint64_t loads[3];
    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
    uint16_t procs, pad;
    uint32_t pad2;
    uint64_t totalhigh, freehigh;
    uint32_t mem_unit;
    char pad3[4];
};

static int64_t l_sysinfo(uint64_t ubuf)
{
    struct sysinfo s;
    memset(&s, 0, sizeof s);
    s.uptime = (int64_t)(timer_uptime_ms() / 1000);
    s.totalram = pmm_total_pages() * PAGE_SIZE;
    s.freeram = pmm_free_count() * PAGE_SIZE;
    struct zk_procinfo info[64];
    s.procs = (uint16_t)proc_snapshot(info, 64);
    s.mem_unit = 1;
    return copy_out(ubuf, &s, sizeof s);
}

static int64_t l_prlimit(int res, uint64_t unew, uint64_t uold)
{
    (void)unew;
    if (!uold)
        return 0;
    uint64_t lim[2] = { UINT64_MAX, UINT64_MAX };
    if (res == 3)            // RLIMIT_STACK
        lim[0] = 8UL << 20;
    else if (res == 7)       // RLIMIT_NOFILE
        lim[0] = lim[1] = PROC_MAX_FDS;
    return copy_out(uold, lim, sizeof lim);
}

struct linux_statfs {
    int64_t type, bsize;
    uint64_t blocks, bfree, bavail, files, ffree;
    int32_t fsid[2];
    int64_t namelen, frsize, flags, spare[4];
};

static int64_t fill_statfs(const char *path, uint64_t ubuf)
{
    struct zk_statfs st;
    int err = vfs_statfs(path, &st);
    if (err)
        return err;
    struct linux_statfs l;
    memset(&l, 0, sizeof l);
    l.type = strcmp(st.fstype, "fat") == 0 ? 0x4d44 : 0x01021994;
    l.bsize = l.frsize = (int64_t)st.block_size;
    l.blocks = st.blocks;
    l.bfree = l.bavail = st.blocks_free;
    l.namelen = 255;
    return copy_out(ubuf, &l, sizeof l);
}

static int64_t l_readlinkat(int dirfd, uint64_t upath, uint64_t ubuf, size_t size)
{
    char path[ZK_PATH_MAX], target[ZK_PATH_MAX];
    char raw[ZK_PATH_MAX];
    int err = get_string(upath, raw, sizeof raw);
    if (err)
        return err;
    if (strcmp(raw, "/proc/self/exe") == 0) {
        strlcpy(target, me()->exe, sizeof target);
        // hide the compat prefix, the program thinks it lives in /
        if (strncmp(target, LINUX_ROOT "/", sizeof LINUX_ROOT) == 0)
            memmove(target, target + sizeof LINUX_ROOT - 1, strlen(target) - sizeof LINUX_ROOT + 2);
    } else {
        err = at_path(dirfd, upath, path, false);
        if (!err)
            err = vfs_readlink(path, target, sizeof target);
        if (err)
            return err;
    }
    size_t n = strlen(target);
    if (n > size)
        n = size;
    return copy_out(ubuf, target, n) ? ZK_EFAULT : (int64_t)n;
}

static int64_t l_access(int dirfd, uint64_t upath, int mode)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, false);
    if (err)
        return err;
    struct vnode *v;
    err = vfs_lookup(path, &v);
    if (err)
        return err;
    int want = (mode & 4 ? MAY_READ : 0) | (mode & 2 ? MAY_WRITE : 0) | (mode & 1 ? MAY_EXEC : 0);
    if (want)
        err = vfs_permission(v, want);
    vnode_put(v);
    return err;
}

static int64_t l_mkdirat(int dirfd, uint64_t upath, uint32_t mode)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, true);
    if (!err)
        err = vfs_mkdir(path);
    if (!err)
        vfs_chmod(path, mode & 07777 & ~022);
    return err;
}

static int64_t l_unlinkat(int dirfd, uint64_t upath, int flags)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, false);
    if (err)
        return err;
    struct zk_stat st;
    err = vfs_lstat(path, &st);
    if (err)
        return err;
    if ((flags & AT_REMOVEDIR) && st.type != ZK_TYPE_DIR)
        return ZK_ENOTDIR;
    if (!(flags & AT_REMOVEDIR) && st.type == ZK_TYPE_DIR)
        return ZK_EISDIR;
    return vfs_unlink(path);
}

static int64_t l_renameat(int ofd, uint64_t uold, int nfd, uint64_t unew)
{
    char from[ZK_PATH_MAX], to[ZK_PATH_MAX];
    int err = at_path(ofd, uold, from, false);
    if (!err)
        err = at_path(nfd, unew, to, true);
    return err ? err : vfs_rename(from, to);
}

static int64_t l_symlinkat(uint64_t utarget, int dirfd, uint64_t upath)
{
    char target[ZK_PATH_MAX], path[ZK_PATH_MAX];
    int err = get_string(utarget, target, sizeof target);
    if (!err)
        err = at_path(dirfd, upath, path, true);
    return err ? err : vfs_symlink(target, path);
}

static int64_t l_chmodat(int dirfd, uint64_t upath, uint32_t mode)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, false);
    return err ? err : vfs_chmod(path, mode);
}

static int64_t l_chownat(int dirfd, uint64_t upath, uint32_t uid, uint32_t gid)
{
    char path[ZK_PATH_MAX];
    int err = at_path(dirfd, upath, path, false);
    return err ? err : vfs_chown(path, uid, gid);
}

static int64_t l_fchmod(int fd, uint32_t mode, bool own, uint32_t uid, uint32_t gid)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    if (!f->path)
        return ZK_EINVAL;
    return own ? vfs_chown(f->path, uid, gid) : vfs_chmod(f->path, mode);
}

static int64_t l_getcwd(uint64_t ubuf, size_t size)
{
    const char *cwd = me()->cwd;
    char shown[ZK_PATH_MAX];
    strlcpy(shown, cwd, sizeof shown);
    size_t n = strlen(shown) + 1;
    if (size < n)
        return -1000 - 34;    // ERANGE
    return copy_out(ubuf, shown, n) ? ZK_EFAULT : (int64_t)n;
}

static int64_t l_chdir_path(const char *path)
{
    struct zk_stat st;
    int err = vfs_stat(path, &st);
    if (err)
        return err;
    if (st.type != ZK_TYPE_DIR)
        return ZK_ENOTDIR;
    strlcpy(me()->cwd, path, ZK_PATH_MAX);
    return 0;
}

static int64_t l_chdir(uint64_t upath)
{
    char path[ZK_PATH_MAX];
    int err = at_path(AT_FDCWD, upath, path, false);
    return err ? err : l_chdir_path(path);
}

static int64_t l_fchdir(int fd)
{
    struct file *f = fd_file(fd);
    if (!f)
        return ZK_EBADF;
    if (!f->path)
        return ZK_ENOTDIR;
    return l_chdir_path(f->path);
}

static int64_t l_dup3(int old, int new, int flags)
{
    struct proc *p = me();
    struct file *f = proc_fd_get(p, old);
    if (!f)
        return ZK_EBADF;
    if (old == new)
        return new;
    file_get(f);
    int r = set_fd(p, new, f);
    if (r < 0) {
        file_put(f);
        return r;
    }
    if (flags & O_CLOEXEC)
        p->cloexec |= 1U << new;
    return new;
}

static int64_t l_pipe2(uint64_t ufds, int flags)
{
    int64_t r = sys_pipe(ufds);
    if (r == 0 && (flags & O_CLOEXEC)) {
        int fds[2];
        copy_in(fds, ufds, sizeof fds);
        me()->cloexec |= (1U << fds[0]) | (1U << fds[1]);
    }
    return r;
}

static int64_t l_ppoll(uint64_t ufds, size_t n, uint64_t uts)
{
    int64_t ms = -1;
    if (uts) {
        struct timespec ts;
        if (copy_in(&ts, uts, sizeof ts))
            return ZK_EFAULT;
        ms = ts.sec * 1000 + ts.nsec / 1000000;
    }
    return sys_poll(ufds, n, ms);
}

static int64_t l_arch_prctl(int code, uint64_t addr)
{
    struct thread *t = thread_current();
    if (code == 0x1002) {    // ARCH_SET_FS
        if (addr >= USER_LIMIT)
            return ZK_EPERM;
        t->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    }
    if (code == 0x1003)      // ARCH_GET_FS
        return copy_out(addr, &t->fs_base, sizeof t->fs_base);
    return ZK_EINVAL;
}

static int64_t l_getgroups(int size, uint64_t ulist)
{
    const struct zk_cred *c = &me()->cred;
    if (size == 0)
        return c->ngroups;
    if ((uint32_t)size < c->ngroups)
        return ZK_EINVAL;
    return copy_out(ulist, c->groups, c->ngroups * 4) ? ZK_EFAULT : (int64_t)c->ngroups;
}

static int64_t l_times(uint64_t ubuf)
{
    uint64_t t = thread_current()->cpu_ticks * 100 / TIMER_HZ;
    if (ubuf) {
        uint64_t tms[4] = { t, 0, 0, 0 };
        if (copy_out(ubuf, tms, sizeof tms))
            return ZK_EFAULT;
    }
    return (int64_t)(timer_uptime_ms() / 10);
}

// the table

#define A1 f->rdi
#define A2 f->rsi
#define A3 f->rdx
#define A4 f->r10
#define A5 f->r8
#define A6 f->r9

static int64_t dispatch(struct interrupt_frame *f)
{
    struct proc *p = me();
    switch (f->rax) {
    case 0:   return l_rw((int)A1, A2, A3, false, -1);
    case 1:   return l_rw((int)A1, A2, A3, true, -1);
    case 2:   return l_openat(AT_FDCWD, A1, (int)A2, (uint32_t)A3);
    case 3:   return l_close((int)A1);
    case 4:   return l_stat_at(AT_FDCWD, A1, A2, 0);
    case 5:   return l_fstat((int)A1, A2);
    case 6:   return l_stat_at(AT_FDCWD, A1, A2, AT_SYMLINK_NOFOLLOW);
    case 7:   return sys_poll(A1, A2, (int64_t)(int32_t)A3);
    case 8: {
        struct file *fl = fd_file((int)A1);
        return fl ? file_seek(fl, (int64_t)A2, (int)A3) : ZK_EBADF;
    }
    case 9:   return l_mmap(A1, A2, (int)A3, (int)A4, (int)A5, A6);
    case 10:  return l_mprotect(A1, A2, (int)A3);
    case 11:  return l_munmap(A1, A2);
    case 12:  return l_brk(A1);
    case 13:  return l_sigaction((int)A1, A2, A3, A4);
    case 14:  return l_sigprocmask((int)A1, A2, A3, A4);
    case 16:  return l_ioctl((int)A1, A2, A3);
    case 17:  return l_rw((int)A1, A2, A3, false, (int64_t)A4);
    case 18:  return l_rw((int)A1, A2, A3, true, (int64_t)A4);
    case 19:  return l_rwv((int)A1, A2, (int)A3, false, -1);
    case 20:  return l_rwv((int)A1, A2, (int)A3, true, -1);
    case 21:  return l_access(AT_FDCWD, A1, (int)A2);
    case 22:  return sys_pipe(A1);
    case 24:  thread_yield(); return 0;
    case 25:  return ZK_ENOMEM;    // mremap, callers fall back to copying
    case 26:  return 0;            // msync
    case 28:  return 0;            // madvise
    case 32: {
        struct file *fl = fd_file((int)A1);
        if (!fl)
            return ZK_EBADF;
        int n = alloc_fd_from(p, 0, fl);
        if (n >= 0)
            file_get(fl);
        return n;
    }
    case 33:  return l_dup3((int)A1, (int)A2, 0);
    case 34:  return wait_signal();    // pause
    case 35:  return l_nanosleep(A1);
    case 37:  return 0;            // alarm
    case 38:  return 0;            // setitimer
    case 39:  return (int64_t)p->pid;
    case 41: case 42: case 43: case 44: case 45: case 46: case 47: case 48:
    case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        return linux_socketcall(f->rax, A1, A2, A3, A4, A5, A6);
    case 56:  return l_clone(f, A1, A2, A3, A4, A5);
    case 57:  return proc_fork(f, 0, 0);
    case 58:  return proc_fork(f, 0, 0);
    case 59:  return l_execve(A1, A2, A3);
    case 60:  proc_thread_exit((int)(A1 & 0xff));
    case 61:  return l_wait4((int64_t)A1, A2, (int)A3, A4);
    case 62:  return l_kill((int64_t)(int32_t)A1, (int)A2, SI_USER);
    case 63:  return l_uname(A1);
    case 72:  return l_fcntl((int)A1, (int)A2, A3);
    case 73:  return 0;            // flock
    case 74:  return 0;            // fsync
    case 75:  return 0;            // fdatasync
    case 76: {
        char path[ZK_PATH_MAX];
        struct file *tf;
        int err = at_path(AT_FDCWD, A1, path, false);
        if (!err)
            err = vfs_open(path, O_WRONLY, &tf);
        if (err)
            return err;
        err = vfs_truncate(tf->v, A2);
        file_put(tf);
        return err;
    }
    case 77: {
        struct file *fl = fd_file((int)A1);
        if (!fl)
            return ZK_EBADF;
        return (fl->flags & O_ACCMODE) == O_RDONLY ? ZK_EINVAL : vfs_truncate(fl->v, A2);
    }
    case 79:  return l_getcwd(A1, A2);
    case 80:  return l_chdir(A1);
    case 81:  return l_fchdir((int)A1);
    case 82:  return l_renameat(AT_FDCWD, A1, AT_FDCWD, A2);
    case 83:  return l_mkdirat(AT_FDCWD, A1, (uint32_t)A2);
    case 84:  return l_unlinkat(AT_FDCWD, A1, AT_REMOVEDIR);
    case 85:  return l_openat(AT_FDCWD, A1, O_WRONLY | O_CREAT | O_TRUNC, (uint32_t)A2);
    case 86:  return ZK_EPERM;     // hard links
    case 87:  return l_unlinkat(AT_FDCWD, A1, 0);
    case 88:  return l_symlinkat(A1, AT_FDCWD, A2);
    case 89:  return l_readlinkat(AT_FDCWD, A1, A2, A3);
    case 90:  return l_chmodat(AT_FDCWD, A1, (uint32_t)A2);
    case 91:  return l_fchmod((int)A1, (uint32_t)A2, false, 0, 0);
    case 92:
    case 94:  return l_chownat(AT_FDCWD, A1, (uint32_t)A2, (uint32_t)A3);
    case 93:  return l_fchmod((int)A1, 0, true, (uint32_t)A2, (uint32_t)A3);
    case 95:  return 022;          // umask
    case 96: {
        int64_t tv[2] = { rtc_now(), (int64_t)(timer_uptime_ms() % 1000) * 1000 };
        return A1 ? copy_out(A1, tv, sizeof tv) : 0;
    }
    case 97:  return l_prlimit((int)A1, 0, A2);
    case 98: {
        uint8_t zero[144] = { 0 };
        return copy_out(A2, zero, sizeof zero);
    }
    case 99:  return l_sysinfo(A1);
    case 100: return l_times(A1);
    case 102: return p->cred.uid;
    case 104: return p->cred.gid;
    case 105: return sys_setuid((uint32_t)A1);
    case 106: return sys_setgid((uint32_t)A1);
    case 107: return p->cred.euid;
    case 108: return p->cred.egid;
    case 109:    // setpgid
        if (A1 == 0 || A1 == p->pid)
            p->pgid = A2 ? A2 : p->pid;
        return 0;
    case 110: return p->parent ? (int64_t)p->parent->pid : 1;
    case 111: return (int64_t)p->pgid;
    case 112: p->pgid = p->pid; return (int64_t)p->pid;    // setsid
    case 115: return l_getgroups((int)A1, A2);
    case 116: return sys_setgroups(A1, A2);
    case 117: return sys_setuid((uint32_t)A2);    // setresuid, close enough
    case 119: return sys_setgid((uint32_t)A2);
    case 118:
    case 120: {    // getresuid, getresgid
        uint32_t ids[3];
        if (f->rax == 118) {
            ids[0] = p->cred.uid; ids[1] = p->cred.euid; ids[2] = p->cred.suid;
        } else {
            ids[0] = p->cred.gid; ids[1] = p->cred.egid; ids[2] = p->cred.sgid;
        }
        if (copy_out(A1, &ids[0], 4) || copy_out(A2, &ids[1], 4) || copy_out(A3, &ids[2], 4))
            return ZK_EFAULT;
        return 0;
    }
    case 121: return (int64_t)p->pgid;
    case 124: return (int64_t)p->pgid;
    case 127: return l_sigpending(A1, A2);
    case 130: return l_sigsuspend(A1, A2);
    case 131: return l_sigaltstack(A1, A2, f->rsp);
    case 137: {
        char path[ZK_PATH_MAX];
        int err = at_path(AT_FDCWD, A1, path, false);
        return err ? err : fill_statfs(path, A2);
    }
    case 138: {
        struct file *fl = fd_file((int)A1);
        if (!fl)
            return ZK_EBADF;
        return fill_statfs(fl->path ? fl->path : "/", A2);
    }
    case 157: return 0;            // prctl
    case 158: return l_arch_prctl((int)A1, A2);
    case 162: vfs_sync(); return 0;
    case 186: return (int64_t)thread_current()->id;    // gettid
    case 200: return l_tkill(0, (int32_t)A1, (int)A2);
    case 234: return l_tkill((int32_t)A1, (int32_t)A2, (int)A3);
    case 201: {
        int64_t t = rtc_now();
        if (A1 && copy_out(A1, &t, sizeof t))
            return ZK_EFAULT;
        return t;
    }
    case 202: return l_futex(A1, (int)A2, (uint32_t)A3, A4, A5, (uint32_t)A6);
    case 204: {    // sched_getaffinity
        uint64_t mask = 1;
        if (A2 < 8)
            return ZK_EINVAL;
        return copy_out(A3, &mask, 8) ? ZK_EFAULT : 8;
    }
    case 213: case 232: case 233: case 281: case 291:
        return ZK_ENOSYS;          // epoll
    case 217: return l_getdents64((int)A1, A2, A3);
    case 218:    // set_tid_address
        thread_current()->clear_tid = A1;
        return (int64_t)thread_current()->id;
    case 228: return l_clock_gettime((int)A1, A2);
    case 229: {
        struct timespec res = { 0, 1000000 };
        return A2 ? copy_out(A2, &res, sizeof res) : 0;
    }
    case 230: return l_nanosleep(A3);
    case 231: proc_exit((int)(A1 & 0xff));
    case 257: return l_openat((int)A1, A2, (int)A3, (uint32_t)A4);
    case 258: return l_mkdirat((int)A1, A2, (uint32_t)A3);
    case 260: return l_chownat((int)A1, A2, (uint32_t)A3, (uint32_t)A4);
    case 262: return l_stat_at((int)A1, A2, A3, (int)A4);
    case 263: return l_unlinkat((int)A1, A2, (int)A3);
    case 264: return l_renameat((int)A1, A2, (int)A3, A4);
    case 265: return ZK_EPERM;     // linkat
    case 266: return l_symlinkat(A1, (int)A2, A3);
    case 267: return l_readlinkat((int)A1, A2, A3, A4);
    case 268: return l_chmodat((int)A1, A2, (uint32_t)A3);
    case 269:
    case 439: return l_access((int)A1, A2, (int)A3);
    case 270: return ZK_ENOSYS;    // pselect6
    case 271: return l_ppoll(A1, A2, A3);
    case 273: return 0;            // set_robust_list
    case 280: return 0;            // utimensat
    case 292: return l_dup3((int)A1, (int)A2, (int)A3);
    case 293: return l_pipe2(A1, (int)A2);
    case 302: return l_prlimit((int)A2, A3, A4);
    case 316: return l_renameat((int)A1, A2, (int)A3, A4);
    case 318: return sys_getrandom(A1, A2);
    case 332: return ZK_ENOSYS;    // statx, everyone falls back to fstatat
    case 334: return ZK_ENOSYS;    // rseq
    case 435: return ZK_ENOSYS;    // clone3, glibc falls back to clone
    }
    return ZK_ENOSYS;
}

void linux_syscall(struct interrupt_frame *f)
{
    sti();
    int64_t nr = (int64_t)f->rax;
    if (nr == 15) {    // rt_sigreturn puts every register back, rax too
        signal_sigreturn(f);
        signal_return(f, -1);
        cli();
        return;
    }
    int64_t r = dispatch(f);
    // -1000 - n is linux errno n straight, for ones zos has no name for
    if (r <= -1000 && r > -1200)
        f->rax = (uint64_t)(r + 1000);
    else
        f->rax = (uint64_t)lerr(r);
    signal_return(f, nr);
    cli();
}

void linux_init(void)
{
    // syscall lands on kernel cs 0x08, sysret would use 0x10 + 16 = user cs 0x23
    wrmsr(MSR_STAR, (0x10UL << 48) | (0x08UL << 32));
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, 0x40700);    // IF, TF, DF and AC off on entry
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1);
}
