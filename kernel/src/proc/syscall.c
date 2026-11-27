#include "proc/proc.h"
#include "proc/sys.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/power.h"
#include "arch/x86_64/interrupts.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "lib/random.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "net/net.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include <zk/syscall.h>

#define MAX_ALLOC_PAGES (64 * 1024)
#define MAX_PS          64

static struct address_space *cur_as(void)
{
    return thread_current()->as;
}

bool user_ok(uint64_t addr, size_t len, bool write)
{
    return vmm_user_range_ok(cur_as(), addr, len, write);
}

int copy_out(uint64_t dst, const void *src, size_t len)
{
    if (!user_ok(dst, len, true))
        return ZK_EFAULT;
    memcpy((void *)dst, src, len);
    return 0;
}

int copy_in(void *dst, uint64_t src, size_t len)
{
    if (!user_ok(src, len, false))
        return ZK_EFAULT;
    memcpy(dst, (const void *)src, len);
    return 0;
}

// copies a nul terminated user string into buf
int get_string(uint64_t addr, char *buf, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        if ((i == 0 || (addr + i) % PAGE_SIZE == 0) && !user_ok(addr + i, 1, false))
            return ZK_EFAULT;
        buf[i] = *(const char *)(addr + i);
        if (!buf[i])
            return 0;
    }
    return ZK_ENAMETOOLONG;
}

// user path -> absolute normalized path
int get_path(uint64_t addr, char *out)
{
    char raw[ZK_PATH_MAX];
    int err = get_string(addr, raw, sizeof raw);
    if (err)
        return err;
    return vfs_normalize(proc_current()->cwd, raw, out);
}

static struct file *get_fd(int fd)
{
    return proc_fd_get(proc_current(), fd);
}

static int64_t sys_write(int fd, uint64_t buf, size_t len)
{
    struct file *f = get_fd(fd);
    if (!f)
        return ZK_EBADF;
    if (!user_ok(buf, len, false))
        return ZK_EFAULT;
    return file_write(f, (const void *)buf, len);
}

static int64_t sys_read(int fd, uint64_t buf, size_t len)
{
    struct file *f = get_fd(fd);
    if (!f)
        return ZK_EBADF;
    if (!user_ok(buf, len, true))
        return ZK_EFAULT;
    return file_read(f, (void *)buf, len);
}

static int64_t sys_alloc_pages(size_t count)
{
    struct address_space *as = cur_as();
    if (count == 0 || count > MAX_ALLOC_PAGES)
        return ZK_EINVAL;

    uint64_t start = as->heap_top;
    for (size_t i = 0; i < count; i++) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p || !vmm_map_user(as, start + i * PAGE_SIZE, p, PTE_WRITE | PTE_NX)) {
            if (p)
                pmm_free(p);
            // pages mapped so far stay with the process and get freed on exit
            as->heap_top = start + i * PAGE_SIZE;
            return ZK_ENOMEM;
        }
    }
    as->heap_top = start + count * PAGE_SIZE;
    return (int64_t)start;
}

static int64_t sys_open(uint64_t upath, int flags)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    if (err)
        return err;
    struct file *f;
    err = vfs_open(path, flags, &f);
    if (err)
        return err;
    int fd = proc_fd_alloc(proc_current(), f);
    if (fd < 0)
        file_put(f);
    return fd;
}

static int64_t sys_close(int fd)
{
    struct proc *p = proc_current();
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return ZK_EBADF;
    p->fds[fd] = NULL;
    file_put(f);
    return 0;
}

static int64_t sys_stat(uint64_t upath, uint64_t ust)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    if (err)
        return err;
    if (!user_ok(ust, sizeof(struct zk_stat), true))
        return ZK_EFAULT;
    return vfs_stat(path, (struct zk_stat *)ust);
}

static int64_t sys_fstat(int fd, uint64_t ust)
{
    struct file *f = get_fd(fd);
    if (!f)
        return ZK_EBADF;
    if (!user_ok(ust, sizeof(struct zk_stat), true))
        return ZK_EFAULT;
    file_stat(f, (struct zk_stat *)ust);
    return 0;
}

static int64_t sys_readdir(int fd, uint64_t ud)
{
    struct file *f = get_fd(fd);
    if (!f)
        return ZK_EBADF;
    if (!user_ok(ud, sizeof(struct zk_dirent), true))
        return ZK_EFAULT;
    return file_readdir(f, (struct zk_dirent *)ud);
}

static int64_t path_op(uint64_t upath, int (*op)(const char *))
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    return err ? err : op(path);
}

static int64_t sys_chdir(uint64_t upath)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    if (err)
        return err;
    struct zk_stat st;
    err = vfs_stat(path, &st);
    if (err)
        return err;
    if (st.type != ZK_TYPE_DIR)
        return ZK_ENOTDIR;
    strlcpy(proc_current()->cwd, path, ZK_PATH_MAX);
    return 0;
}

static int64_t sys_getcwd(uint64_t buf, size_t size)
{
    const char *cwd = proc_current()->cwd;
    size_t len = strlen(cwd);
    if (size < len + 1)
        return ZK_EINVAL;
    if (!user_ok(buf, len + 1, true))
        return ZK_EFAULT;
    memcpy((void *)buf, cwd, len + 1);
    return (int64_t)len;
}


static int get_strv(uint64_t uv, struct strv *v, int max)
{
    int n = 0;
    v->ptrs[0] = NULL;
    while (uv) {
        uint64_t slot = uv + (uint64_t)n * sizeof(uint64_t);
        if (!user_ok(slot, sizeof(uint64_t), false))
            return ZK_EFAULT;
        uint64_t s = *(const uint64_t *)slot;
        if (!s)
            break;
        if (n == max)
            return ZK_E2BIG;
        size_t room = PROC_MAX_ARG_BYTES - v->used;
        if (room < 2)
            return ZK_E2BIG;
        int err = get_string(s, v->buf + v->used, room);
        if (err)
            return err == ZK_ENAMETOOLONG ? ZK_E2BIG : err;
        v->ptrs[n++] = v->buf + v->used;
        v->used += strlen(v->buf + v->used) + 1;
    }
    v->ptrs[n] = NULL;
    return 0;
}


// path, argv and envp in one allocation, free with kfree
int get_exec_args(uint64_t upath, uint64_t uargv, uint64_t uenvp, struct exec_args **out)
{
    struct exec_args *a = kmalloc(sizeof(*a) + PROC_MAX_ARG_BYTES);
    if (!a)
        return ZK_ENOMEM;
    a->argv.buf = a->envp.buf = a->buf;
    a->argv.used = a->envp.used = 0;
    int err = get_path(upath, a->path);
    if (!err)
        err = get_strv(uargv, &a->argv, PROC_MAX_ARGS);
    if (!err) {
        a->envp.used = a->argv.used;
        err = get_strv(uenvp, &a->envp, PROC_MAX_ENV);
    }
    if (err) {
        kfree(a);
        return err;
    }
    *out = a;
    return 0;
}

static int64_t sys_spawn(uint64_t upath, uint64_t uargv, uint64_t uopts, uint64_t uenvp)
{
    struct proc *me = proc_current();
    struct zk_spawn opts = { { 0, 1, 2 }, 0 };
    if (uopts) {
        if (!user_ok(uopts, sizeof opts, false))
            return ZK_EFAULT;
        memcpy(&opts, (const void *)uopts, sizeof opts);
    }

    struct spawn_opts o = { .parent = me, .pgid = opts.pgid };
    for (int i = 0; i < 3; i++) {
        if (opts.fd[i] == -1)
            continue;
        o.std[i] = proc_fd_get(me, opts.fd[i]);
        if (!o.std[i])
            return ZK_EBADF;
    }

    struct exec_args *a;
    int err = get_exec_args(upath, uargv, uenvp, &a);
    if (err)
        return err;
    struct thread *t = proc_spawn(a->path, a->argv.ptrs, a->envp.ptrs, &o, &err);
    kfree(a);
    return t ? (int64_t)t->id : err;
}

static int64_t sys_exec(uint64_t upath, uint64_t uargv, uint64_t uenvp)
{
    struct exec_args *a;
    int err = get_exec_args(upath, uargv, uenvp, &a);
    if (err)
        return err;
    err = proc_exec(a->path, a->argv.ptrs, a->envp.ptrs, a);
    kfree(a);
    return err;
}

static int64_t sys_wait(int64_t pid, uint64_t ustatus, int flags)
{
    if (ustatus && !user_ok(ustatus, sizeof(int), true))
        return ZK_EFAULT;
    int status = 0;
    int64_t r = proc_wait(pid, &status, flags);
    if (r > 0 && ustatus) {
        status = PROC_STATUS_CODE(status);
        if (copy_out(ustatus, &status, sizeof status))
            return ZK_EFAULT;
    }
    return r;
}

int64_t sys_pipe(uint64_t ufds)
{
    if (!user_ok(ufds, 2 * sizeof(int), true))
        return ZK_EFAULT;
    struct proc *p = proc_current();
    struct file *rd, *wr;
    int err = pipe_create(&rd, &wr);
    if (err)
        return err;
    int a = proc_fd_alloc(p, rd);
    int b = a < 0 ? a : proc_fd_alloc(p, wr);
    if (b < 0) {
        if (a >= 0)
            p->fds[a] = NULL;
        file_put(rd);
        file_put(wr);
        return ZK_EMFILE;
    }
    ((int *)ufds)[0] = a;
    ((int *)ufds)[1] = b;
    return 0;
}

static int64_t sys_dup(int fd)
{
    struct proc *p = proc_current();
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return ZK_EBADF;
    int n = proc_fd_alloc(p, f);
    if (n >= 0)
        file_get(f);
    return n;
}

static int64_t sys_ps(uint64_t out, size_t max)
{
    if (max > MAX_PS)
        max = MAX_PS;
    if (!user_ok(out, max * sizeof(struct zk_procinfo), true))
        return ZK_EFAULT;
    struct zk_procinfo *tmp = kmalloc(MAX_PS * sizeof(*tmp));
    if (!tmp)
        return ZK_ENOMEM;
    int n = proc_snapshot(tmp, (int)max);
    memcpy((void *)out, tmp, (size_t)n * sizeof(*tmp));
    kfree(tmp);
    return n;
}

static int64_t sys_rename(uint64_t ufrom, uint64_t uto)
{
    char from[ZK_PATH_MAX], to[ZK_PATH_MAX];
    int err = get_path(ufrom, from);
    if (!err)
        err = get_path(uto, to);
    return err ? err : vfs_rename(from, to);
}

static bool is_root(void)
{
    return proc_current()->cred.euid == 0;
}

static int64_t sys_mount(uint64_t usrc, uint64_t utarget, uint64_t utype, uint64_t uopts)
{
    if (!is_root())
        return ZK_EPERM;
    char src[ZK_PATH_MAX], target[ZK_PATH_MAX], type[16], opts[64] = "";
    int err = get_string(usrc, src, sizeof src);
    if (!err && uopts)
        err = get_string(uopts, opts, sizeof opts);
    if (!err)
        err = get_path(utarget, target);
    if (!err)
        err = get_string(utype, type, sizeof type);
    if (err)
        return err;
    // device sources are paths too
    if (src[0] && strcmp(src, "none") != 0) {
        char abs[ZK_PATH_MAX];
        err = vfs_normalize(proc_current()->cwd, src, abs);
        if (err)
            return err;
        strlcpy(src, abs, sizeof src);
    }
    return vfs_mount(src, target, type, opts);
}

int64_t sys_statfs(uint64_t upath, uint64_t ust)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    if (err)
        return err;
    if (!user_ok(ust, sizeof(struct zk_statfs), true))
        return ZK_EFAULT;
    struct zk_statfs st;
    err = vfs_statfs(path, &st);
    if (!err)
        memcpy((void *)ust, &st, sizeof st);
    return err;
}

static int64_t sys_ftruncate(int fd, uint64_t size)
{
    struct file *f = get_fd(fd);
    if (!f)
        return ZK_EBADF;
    if ((f->flags & O_ACCMODE) == O_RDONLY)
        return ZK_EBADF;
    return vfs_truncate(f->v, size);
}

static int64_t sys_ioctl(int fd, unsigned long req, uint64_t arg)
{
    struct file *f = get_fd(fd);
    return f ? file_ioctl(f, req, arg) : ZK_EBADF;
}

#define MAX_POLL 64

int64_t sys_poll(uint64_t ufds, size_t n, int64_t timeout)
{
    if (n > MAX_POLL)
        return ZK_EINVAL;
    struct zk_pollfd fds[MAX_POLL];
    if (n && copy_in(fds, ufds, n * sizeof(fds[0])))
        return ZK_EFAULT;
    struct proc *me = proc_current();
    uint64_t deadline = timeout > 0 ? timer_uptime_ms() + (uint64_t)timeout : 0;

    int ready = 0;
    // interrupts stay off between checking and sleeping, so no wakeup is missed
    uint64_t flags = irq_save();
    for (;;) {
        ready = 0;
        for (size_t i = 0; i < n; i++) {
            fds[i].revents = 0;
            if (fds[i].fd < 0)
                continue;
            struct file *f = proc_fd_get(me, fds[i].fd);
            int r = f ? file_poll(f) : POLLNVAL;
            fds[i].revents = (int16_t)(r & (fds[i].events | POLLHUP | POLLNVAL));
            if (fds[i].revents)
                ready++;
        }
        if (ready || timeout == 0)
            break;
        if (proc_killed()) {
            ready = ZK_EINTR;
            break;
        }
        int64_t left = -1;
        if (timeout > 0) {
            uint64_t now = timer_uptime_ms();
            if (now >= deadline)
                break;
            left = (int64_t)(deadline - now);
        }
        poll_wait(left);
    }
    irq_restore(flags);
    if (n && copy_out(ufds, fds, n * sizeof(fds[0])))
        return ZK_EFAULT;
    return ready;
}

static int64_t sys_openpty(uint64_t ufds)
{
    if (!user_ok(ufds, 2 * sizeof(int), true))
        return ZK_EFAULT;
    struct proc *p = proc_current();
    struct file *m, *s;
    int err = pty_create(&m, &s);
    if (err)
        return err;
    int a = proc_fd_alloc(p, m);
    int b = a < 0 ? a : proc_fd_alloc(p, s);
    if (b < 0) {
        if (a >= 0)
            p->fds[a] = NULL;
        file_put(m);
        file_put(s);
        return ZK_EMFILE;
    }
    ((int *)ufds)[0] = a;
    ((int *)ufds)[1] = b;
    return 0;
}

int64_t sys_setuid(uint32_t uid)
{
    struct zk_cred *c = &proc_current()->cred;
    if (c->euid == 0) {
        c->uid = c->euid = c->suid = uid;
        return 0;
    }
    if (uid != c->uid && uid != c->suid)
        return ZK_EPERM;
    c->euid = uid;
    return 0;
}

int64_t sys_setgid(uint32_t gid)
{
    struct zk_cred *c = &proc_current()->cred;
    if (c->euid == 0) {
        c->gid = c->egid = c->sgid = gid;
        return 0;
    }
    if (gid != c->gid && gid != c->sgid)
        return ZK_EPERM;
    c->egid = gid;
    return 0;
}

int64_t sys_setgroups(size_t n, uint64_t ugroups)
{
    struct zk_cred *c = &proc_current()->cred;
    if (c->euid != 0)
        return ZK_EPERM;
    if (n > ZK_NGROUPS)
        return ZK_EINVAL;
    uint32_t g[ZK_NGROUPS];
    if (n && copy_in(g, ugroups, n * sizeof(uint32_t)))
        return ZK_EFAULT;
    memcpy(c->groups, g, n * sizeof(uint32_t));
    c->ngroups = (uint32_t)n;
    return 0;
}

static int64_t sys_chmod(uint64_t upath, uint32_t mode)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    return err ? err : vfs_chmod(path, mode);
}

static int64_t sys_chown(uint64_t upath, uint32_t uid, uint32_t gid)
{
    char path[ZK_PATH_MAX];
    int err = get_path(upath, path);
    return err ? err : vfs_chown(path, uid, gid);
}

int64_t sys_kill(uint64_t pid)
{
    // only your own processes, unless you're root
    struct zk_procinfo info[64];
    int n = proc_snapshot(info, 64);
    const struct zk_cred *c = cred_current();
    for (int i = 0; i < n; i++)
        if (info[i].pid == pid && c->euid != 0 && info[i].uid != c->uid && info[i].uid != c->euid)
            return ZK_EPERM;
    return proc_kill(pid);
}

int64_t sys_getrandom(uint64_t ubuf, size_t len)
{
    if (len > 4096)
        len = 4096;
    if (!user_ok(ubuf, len, true))
        return ZK_EFAULT;
    random_bytes((void *)ubuf, len);
    return (int64_t)len;
}

static void syscall_handler(struct interrupt_frame *f)
{
    // syscalls can take a while, let interrupts in
    sti();

    uint64_t a = f->rdi, b = f->rsi, c = f->rdx, d = f->r10, e = f->r8, g = f->r9;
    int64_t ret;
    switch (f->rax) {
    case SYS_EXIT:       proc_exit((int)a);
    case SYS_WRITE:      ret = sys_write((int)a, b, c); break;
    case SYS_READ:       ret = sys_read((int)a, b, c); break;
    case SYS_GETPID:     ret = (int64_t)proc_current()->pid; break;
    case SYS_SLEEP_MS:   thread_sleep_ms(a); ret = 0; break;
    case SYS_YIELD:      thread_yield(); ret = 0; break;
    case SYS_UPTIME_MS:  ret = (int64_t)timer_uptime_ms(); break;
    case SYS_ALLOC_PAGES: ret = sys_alloc_pages(a); break;
    case SYS_OPEN:       ret = sys_open(a, (int)b); break;
    case SYS_CLOSE:      ret = sys_close((int)a); break;
    case SYS_SEEK: {
        struct file *file = get_fd((int)a);
        ret = file ? file_seek(file, (int64_t)b, (int)c) : ZK_EBADF;
        break;
    }
    case SYS_STAT:       ret = sys_stat(a, b); break;
    case SYS_FSTAT:      ret = sys_fstat((int)a, b); break;
    case SYS_READDIR:    ret = sys_readdir((int)a, b); break;
    case SYS_MKDIR:      ret = path_op(a, vfs_mkdir); break;
    case SYS_UNLINK:     ret = path_op(a, vfs_unlink); break;
    case SYS_CHDIR:      ret = sys_chdir(a); break;
    case SYS_GETCWD:     ret = sys_getcwd(a, b); break;
    case SYS_SPAWN:      ret = sys_spawn(a, b, c, d); break;
    case SYS_FORK:       ret = proc_fork(f, 0, 0); break;
    case SYS_RENAME:     ret = sys_rename(a, b); break;
    case SYS_TIME:       ret = rtc_now(); break;
    case SYS_MOUNT:      ret = sys_mount(a, b, c, d); break;
    case SYS_UMOUNT:     ret = is_root() ? path_op(a, vfs_umount) : ZK_EPERM; break;
    case SYS_SYNC:       vfs_sync(); ret = 0; break;
    case SYS_STATFS:     ret = sys_statfs(a, b); break;
    case SYS_FTRUNCATE:  ret = sys_ftruncate((int)a, b); break;
    case SYS_IOCTL:      ret = sys_ioctl((int)a, b, c); break;
    case SYS_POLL:       ret = sys_poll(a, b, (int64_t)c); break;
    case SYS_OPENPTY:    ret = sys_openpty(a); break;
    case SYS_GETCRED:    ret = copy_out(a, &proc_current()->cred, sizeof(struct zk_cred)); break;
    case SYS_SETUID:     ret = sys_setuid((uint32_t)a); break;
    case SYS_SETGID:     ret = sys_setgid((uint32_t)a); break;
    case SYS_SETGROUPS:  ret = sys_setgroups(a, b); break;
    case SYS_CHMOD:      ret = sys_chmod(a, (uint32_t)b); break;
    case SYS_CHOWN:      ret = sys_chown(a, (uint32_t)b, (uint32_t)c); break;
    case SYS_GETRANDOM:  ret = sys_getrandom(a, b); break;
    case SYS_SYMLINK: {
        char target[ZK_PATH_MAX], path[ZK_PATH_MAX];
        ret = get_string(a, target, sizeof target);
        if (!ret)
            ret = get_path(b, path);
        if (!ret)
            ret = vfs_symlink(target, path);
        break;
    }
    case SYS_READLINK: {
        char path[ZK_PATH_MAX], target[ZK_PATH_MAX];
        ret = get_path(a, path);
        if (!ret)
            ret = vfs_readlink(path, target, sizeof target);
        if (!ret) {
            size_t n = strlen(target) + 1;
            ret = n > c ? ZK_ENAMETOOLONG : copy_out(b, target, n) ? ZK_EFAULT : (int64_t)n - 1;
        }
        break;
    }
    case SYS_LSTAT: {
        char path[ZK_PATH_MAX];
        struct zk_stat st;
        ret = get_path(a, path);
        if (!ret)
            ret = vfs_lstat(path, &st);
        if (!ret)
            ret = copy_out(b, &st, sizeof st);
        break;
    }
    case SYS_SOCKET:     ret = sys_socket((int)a, (int)b); break;
    case SYS_CONNECT:    ret = sys_connect((int)a, b, (uint32_t)c); break;
    case SYS_BIND:       ret = sys_bind((int)a, b, (uint32_t)c); break;
    case SYS_SENDTO:     ret = sys_sendto((int)a, b, c, (int)d, e, (uint32_t)g); break;
    case SYS_RECVFROM:   ret = sys_recvfrom((int)a, b, c, (int)d, e, g); break;
    case SYS_GETSOCKNAME: ret = sys_getsockname((int)a, b, c, d != 0); break;
    case SYS_SHUTDOWN:   ret = sys_shutdown((int)a, (int)b); break;
    case SYS_NETINFO: {
        struct zk_netinfo ni;
        memcpy(ni.mac, netif.mac, 6);
        ni.up = netif.up;
        ni.pad = 0;
        ni.ip = netif.ip;
        ni.mask = netif.mask;
        ni.gw = netif.gw;
        ni.dns = netif.dns;
        ni.rx_packets = netif.rx_packets;
        ni.tx_packets = netif.tx_packets;
        ni.rx_bytes = netif.rx_bytes;
        ni.tx_bytes = netif.tx_bytes;
        ret = copy_out(a, &ni, sizeof ni);
        break;
    }
    case SYS_PING:       ret = net_ping((uint32_t)a, b > 10000 ? 10000 : b); break;
    case SYS_REBOOT:
        if (!is_root()) {
            ret = ZK_EPERM;
            break;
        }
        if (a == ZK_POWEROFF)
            machine_poweroff();
        if (a == ZK_REBOOT)
            machine_reboot();
        ret = ZK_EINVAL;
        break;
    case SYS_EXEC:       ret = sys_exec(a, b, c); break;
    case SYS_WAIT:       ret = sys_wait((int64_t)a, b, (int)c); break;
    case SYS_PIPE:       ret = sys_pipe(a); break;
    case SYS_DUP:        ret = sys_dup((int)a); break;
    case SYS_KILL:       ret = sys_kill(a); break;
    case SYS_TTY_SETFG:  ret = sys_ioctl(0, TIOCSFG, a); break;
    case SYS_PS:         ret = sys_ps(a, b); break;
    default:             ret = ZK_ENOSYS; break;
    }

    f->rax = (uint64_t)ret;
    cli();
}

void syscall_init(void)
{
    idt_set_handler(ZK_SYSCALL_VECTOR, syscall_handler);
    idt_allow_user(ZK_SYSCALL_VECTOR);
}
