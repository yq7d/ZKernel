#include "zk.h"
#include "lib/printf.h"

#define PAGE_SIZE 4096

char **environ;

static inline long syscall3(long n, long a, long b, long c)
{
    long ret;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c)
                      : "memory");
    return ret;
}

static inline long syscall4(long n, long a, long b, long c, long d)
{
    long ret;
    register long r10 __asm__("r10") = d;
    __asm__ volatile ("int $0x80"
                      : "=a"(ret)
                      : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10)
                      : "memory");
    return ret;
}

void exit(int code)
{
    syscall3(SYS_EXIT, code, 0, 0);
    for (;;)
        ;
}

long write(int fd, const void *buf, size_t len)
{
    return syscall3(SYS_WRITE, fd, (long)buf, (long)len);
}

long read(int fd, void *buf, size_t len)
{
    return syscall3(SYS_READ, fd, (long)buf, (long)len);
}

long getpid(void)
{
    return syscall3(SYS_GETPID, 0, 0, 0);
}

void sleep_ms(uint64_t ms)
{
    syscall3(SYS_SLEEP_MS, (long)ms, 0, 0);
}

void yield(void)
{
    syscall3(SYS_YIELD, 0, 0, 0);
}

uint64_t uptime_ms(void)
{
    return (uint64_t)syscall3(SYS_UPTIME_MS, 0, 0, 0);
}

void *alloc_pages(size_t count)
{
    long r = syscall3(SYS_ALLOC_PAGES, (long)count, 0, 0);
    return r < 0 ? NULL : (void *)r;
}

int open(const char *path, int flags) { return (int)syscall3(SYS_OPEN, (long)path, flags, 0); }
int close(int fd) { return (int)syscall3(SYS_CLOSE, fd, 0, 0); }
long seek(int fd, long off, int whence) { return syscall3(SYS_SEEK, fd, off, whence); }
int stat(const char *path, struct zk_stat *st) { return (int)syscall3(SYS_STAT, (long)path, (long)st, 0); }
int fstat(int fd, struct zk_stat *st) { return (int)syscall3(SYS_FSTAT, fd, (long)st, 0); }
int readdir(int fd, struct zk_dirent *d) { return (int)syscall3(SYS_READDIR, fd, (long)d, 0); }
int mkdir(const char *path) { return (int)syscall3(SYS_MKDIR, (long)path, 0, 0); }
int unlink(const char *path) { return (int)syscall3(SYS_UNLINK, (long)path, 0, 0); }
int chdir(const char *path) { return (int)syscall3(SYS_CHDIR, (long)path, 0, 0); }
int getcwd(char *buf, size_t size) { return (int)syscall3(SYS_GETCWD, (long)buf, (long)size, 0); }
int pipe(int fds[2]) { return (int)syscall3(SYS_PIPE, (long)fds, 0, 0); }
int dup(int fd) { return (int)syscall3(SYS_DUP, fd, 0, 0); }
int kill(long pid) { return (int)syscall3(SYS_KILL, pid, 0, 0); }
void tty_setfg(long pgid) { ioctl(0, TIOCSFG, (uint64_t)pgid); }
int ps(struct zk_procinfo *out, size_t max) { return (int)syscall3(SYS_PS, (long)out, (long)max, 0); }

long spawn(const char *path, char **argv, const struct zk_spawn *opts)
{
    return spawnve(path, argv, opts, environ);
}

long spawnve(const char *path, char **argv, const struct zk_spawn *opts, char **envp)
{
    return syscall4(SYS_SPAWN, (long)path, (long)argv, (long)opts, (long)envp);
}

long fork(void) { return syscall3(SYS_FORK, 0, 0, 0); }

int execve(const char *path, char **argv, char **envp)
{
    return (int)syscall3(SYS_EXEC, (long)path, (long)argv, (long)envp);
}

int execv(const char *path, char **argv)
{
    return execve(path, argv, environ);
}

int rename(const char *from, const char *to) { return (int)syscall3(SYS_RENAME, (long)from, (long)to, 0); }
long time(void) { return syscall3(SYS_TIME, 0, 0, 0); }
int mount(const char *src, const char *target, const char *fstype, const char *opts)
{
    return (int)syscall4(SYS_MOUNT, (long)src, (long)target, (long)fstype, (long)opts);
}
int umount(const char *target) { return (int)syscall3(SYS_UMOUNT, (long)target, 0, 0); }
void sync(void) { syscall3(SYS_SYNC, 0, 0, 0); }
int statfs(const char *path, struct zk_statfs *st) { return (int)syscall3(SYS_STATFS, (long)path, (long)st, 0); }
int ioctl(int fd, unsigned long req, uint64_t arg) { return (int)syscall3(SYS_IOCTL, fd, (long)req, (long)arg); }
int poll(struct zk_pollfd *fds, size_t n, long timeout_ms) { return (int)syscall3(SYS_POLL, (long)fds, (long)n, timeout_ms); }
int getcred(struct zk_cred *c) { return (int)syscall3(SYS_GETCRED, (long)c, 0, 0); }
uint32_t getuid(void) { struct zk_cred c; getcred(&c); return c.uid; }
uint32_t geteuid(void) { struct zk_cred c; getcred(&c); return c.euid; }
uint32_t getgid(void) { struct zk_cred c; getcred(&c); return c.gid; }
int setuid(uint32_t uid) { return (int)syscall3(SYS_SETUID, uid, 0, 0); }
int setgid(uint32_t gid) { return (int)syscall3(SYS_SETGID, gid, 0, 0); }
int setgroups(size_t n, const uint32_t *groups) { return (int)syscall3(SYS_SETGROUPS, (long)n, (long)groups, 0); }
int chmod(const char *path, uint32_t mode) { return (int)syscall3(SYS_CHMOD, (long)path, mode, 0); }
int chown(const char *path, uint32_t uid, uint32_t gid) { return (int)syscall3(SYS_CHOWN, (long)path, uid, gid); }
int symlink(const char *target, const char *path) { return (int)syscall3(SYS_SYMLINK, (long)target, (long)path, 0); }
long readlink(const char *path, char *buf, size_t size) { return syscall3(SYS_READLINK, (long)path, (long)buf, (long)size); }
int lstat(const char *path, struct zk_stat *st) { return (int)syscall3(SYS_LSTAT, (long)path, (long)st, 0); }
long getrandom(void *buf, size_t len) { return syscall3(SYS_GETRANDOM, (long)buf, (long)len, 0); }
int openpty(int fds[2]) { return (int)syscall3(SYS_OPENPTY, (long)fds, 0, 0); }
int reboot(int how) { return (int)syscall3(SYS_REBOOT, how, 0, 0); }
int ftruncate(int fd, uint64_t size) { return (int)syscall3(SYS_FTRUNCATE, fd, (long)size, 0); }

long wait(long pid, int *status, int flags)
{
    return syscall3(SYS_WAIT, pid, (long)status, flags);
}

const char *strerror(int err)
{
    static const char *const names[] = {
        "success", "bad address", "invalid argument", "out of memory",
        "not implemented", "no such file or directory", "bad file descriptor",
        "already exists", "not a directory", "is a directory",
        "directory not empty", "too many open files", "no child processes",
        "not an executable", "name too long", "broken pipe", "interrupted",
        "no such process", "file too big", "not permitted",
        "argument list too long", "busy", "cross-device link",
        "no space left on device", "i/o error", "read-only file system",
        "no such device", "permission denied",
    };
    if (err > 0 || -err > ZK_ERRNO_MAX)
        return "unknown error";
    return names[-err];
}

long write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;
    while (done < len) {
        long n = write(fd, p + done, len - done);
        if (n <= 0)
            return n < 0 ? n : ZK_EINVAL;
        done += (size_t)n;
    }
    return (long)done;
}

struct outbuf {
    char data[256];
    size_t len;
    int fd;
};

static void flush(struct outbuf *o)
{
    if (o->len)
        write_all(o->fd, o->data, o->len);
    o->len = 0;
}

static void buf_putc(char c, void *ctx)
{
    struct outbuf *o = ctx;
    o->data[o->len++] = c;
    if (o->len == sizeof o->data)
        flush(o);
}

int vdprintf(int fd, const char *fmt, va_list ap)
{
    struct outbuf o = { .len = 0, .fd = fd };
    int n = kvformat(buf_putc, &o, fmt, ap);
    flush(&o);
    return n;
}

int dprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(fd, fmt, ap);
    va_end(ap);
    return n;
}

int printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vdprintf(1, fmt, ap);
    va_end(ap);
    return n;
}

int puts(const char *s)
{
    size_t len = strlen(s);
    write_all(1, s, len);
    write_all(1, "\n", 1);
    return (int)len + 1;
}

// the tty does the line editing, this just buffers whatever stdin is
static char in_buf[512];
static size_t in_pos, in_len;

int getchar(void)
{
    if (in_pos == in_len) {
        long n = read(0, in_buf, sizeof in_buf);
        if (n <= 0)
            return -1;
        in_pos = 0;
        in_len = (size_t)n;
    }
    return (unsigned char)in_buf[in_pos++];
}

long readline(char *buf, size_t size)
{
    size_t len = 0;
    int c = -1;
    while ((c = getchar()) >= 0 && c != '\n')
        if (len + 1 < size)
            buf[len++] = (char)c;
    buf[len] = '\0';
    if (c < 0 && len == 0)
        return -1;
    return (long)len;
}

int atoi(const char *s)
{
    int sign = 1, v = 0;
    while (*s == ' ')
        s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (*s++ - '0');
    return v * sign;
}
