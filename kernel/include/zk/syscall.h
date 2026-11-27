#pragma once
// shared between the kernel and user programs
//
// int 0x80, number in rax, args in rdi rsi rdx r10 r8 r9, result in rax.
// negative results are errors
#include <stdint.h>

#define ZK_SYSCALL_VECTOR 0x80

#define SYS_EXIT        0    // (int code)
#define SYS_WRITE       1    // (int fd, const void *buf, size_t len) -> written
#define SYS_READ        2    // (int fd, void *buf, size_t len) -> read, 0 at eof
#define SYS_GETPID      3    // () -> pid
#define SYS_SLEEP_MS    4    // (uint64_t ms)
#define SYS_YIELD       5    // ()
#define SYS_UPTIME_MS   6    // () -> ms since boot
#define SYS_ALLOC_PAGES 7    // (size_t count) -> address of zeroed pages
#define SYS_OPEN        8    // (const char *path, int flags) -> fd
#define SYS_CLOSE       9    // (int fd)
#define SYS_SEEK        10   // (int fd, int64_t off, int whence) -> new offset
#define SYS_STAT        11   // (const char *path, struct zk_stat *st)
#define SYS_FSTAT       12   // (int fd, struct zk_stat *st)
#define SYS_READDIR     13   // (int fd, struct zk_dirent *d) -> 1, or 0 at the end
#define SYS_MKDIR       14   // (const char *path)
#define SYS_UNLINK      15   // (const char *path), files and empty dirs
#define SYS_CHDIR       16   // (const char *path)
#define SYS_GETCWD      17   // (char *buf, size_t size) -> length
#define SYS_SPAWN       18   // (const char *path, char **argv, const struct zk_spawn *opts, char **envp) -> pid
#define SYS_WAIT        19   // (int64_t pid, int *status, int flags) -> pid, pid -1 for any child
#define SYS_PIPE        20   // (int fds[2]), fds[0] reads, fds[1] writes
#define SYS_DUP         21   // (int fd) -> new fd
#define SYS_KILL        22   // (int64_t pid)
#define SYS_TTY_SETFG   23   // (uint64_t pgid), same as ioctl(0, TIOCSFG, pgid)
#define SYS_PS          24   // (struct zk_procinfo *out, size_t max) -> count
#define SYS_FORK        25   // () -> child pid, 0 in the child
#define SYS_EXEC        26   // (const char *path, char **argv, char **envp), only returns on error
#define SYS_RENAME      27   // (const char *from, const char *to)
#define SYS_TIME        28   // () -> seconds since 1970, utc
#define SYS_MOUNT       29   // (const char *source, const char *target, const char *fstype, const char *opts)
#define SYS_UMOUNT      30   // (const char *target)
#define SYS_SYNC        31   // ()
#define SYS_STATFS      32   // (const char *path, struct zk_statfs *st)
#define SYS_FTRUNCATE   33   // (int fd, uint64_t size)
#define SYS_REBOOT      34   // (int how), ZK_REBOOT or ZK_POWEROFF. syncs first
#define SYS_IOCTL       35   // (int fd, unsigned long req, uint64_t arg)
#define SYS_POLL        36   // (struct zk_pollfd *fds, size_t n, int64_t timeout_ms) -> ready count
#define SYS_OPENPTY     37   // (int fds[2]), fds[0] is the master, fds[1] the slave
#define SYS_GETCRED     38   // (struct zk_cred *out)
#define SYS_SETUID      39   // (uint32_t uid), root sets all three ids, others only euid
#define SYS_SETGID      40   // (uint32_t gid), same rules
#define SYS_SETGROUPS   41   // (size_t n, const uint32_t *groups), root only
#define SYS_CHMOD       42   // (const char *path, uint32_t mode)
#define SYS_CHOWN       43   // (const char *path, uint32_t uid, uint32_t gid), root only, -1 keeps one
#define SYS_GETRANDOM   44   // (void *buf, size_t len) -> len
#define SYS_SYMLINK     45   // (const char *target, const char *path)
#define SYS_READLINK    46   // (const char *path, char *buf, size_t size) -> length
#define SYS_LSTAT       47   // (const char *path, struct zk_stat *st)
#define SYS_SOCKET      48   // (int domain, int type), AF_INET with SOCK_STREAM or SOCK_DGRAM
#define SYS_CONNECT     49   // (int fd, const struct zk_sockaddr_in *addr, uint32_t len)
#define SYS_BIND        50   // (int fd, const struct zk_sockaddr_in *addr, uint32_t len)
#define SYS_SENDTO      51   // (int fd, buf, len, flags, addr, addrlen)
#define SYS_RECVFROM    52   // (int fd, buf, len, flags, addr, uint32_t *addrlen)
#define SYS_GETSOCKNAME 53   // (int fd, addr, uint32_t *addrlen, int peer)
#define SYS_SHUTDOWN    54   // (int fd, int how)
#define SYS_NETINFO     55   // (struct zk_netinfo *out)
#define SYS_PING        56   // (uint32_t ip, uint64_t timeout_ms) -> round trip ms

#define ZK_REBOOT   0
#define ZK_POWEROFF 1

#define ZK_EFAULT       (-1)
#define ZK_EINVAL       (-2)
#define ZK_ENOMEM       (-3)
#define ZK_ENOSYS       (-4)
#define ZK_ENOENT       (-5)
#define ZK_EBADF        (-6)
#define ZK_EEXIST       (-7)
#define ZK_ENOTDIR      (-8)
#define ZK_EISDIR       (-9)
#define ZK_ENOTEMPTY    (-10)
#define ZK_EMFILE       (-11)
#define ZK_ECHILD       (-12)
#define ZK_ENOEXEC      (-13)
#define ZK_ENAMETOOLONG (-14)
#define ZK_EPIPE        (-15)
#define ZK_EINTR        (-16)
#define ZK_ESRCH        (-17)
#define ZK_EFBIG        (-18)
#define ZK_EPERM        (-19)
#define ZK_E2BIG        (-20)
#define ZK_EBUSY        (-21)
#define ZK_EXDEV        (-22)
#define ZK_ENOSPC       (-23)
#define ZK_EIO          (-24)
#define ZK_EROFS        (-25)
#define ZK_ENODEV       (-26)
#define ZK_EACCES       (-27)
#define ZK_ELOOP        (-28)
#define ZK_ENOTTY       (-29)
#define ZK_EAGAIN       (-30)
#define ZK_ENOTSOCK     (-31)
#define ZK_ECONNREFUSED (-32)
#define ZK_ETIMEDOUT    (-33)
#define ZK_ENETUNREACH  (-34)
#define ZK_EADDRINUSE   (-35)
#define ZK_ENOTCONN     (-36)
#define ZK_ECONNRESET   (-37)
#define ZK_EHOSTUNREACH (-38)
#define ZK_EINPROGRESS  (-39)
#define ZK_ERRNO_MAX    39

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0x40
#define O_EXCL    0x80
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define O_NONBLOCK  0x800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

#define WNOHANG 1    // wait returns 0 instead of blocking

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define ZK_PATH_MAX 256
#define ZK_NAME_MAX 59

#define ZK_TYPE_FILE 1
#define ZK_TYPE_DIR  2
#define ZK_TYPE_DEV  3
#define ZK_TYPE_PIPE 4
#define ZK_TYPE_LINK 5
#define ZK_TYPE_SOCK 6

struct zk_stat {
    uint32_t type;
    uint32_t mode;    // permission bits, plus ZK_S_ISUID and friends
    uint64_t size;
    uint64_t ino;
    int64_t mtime;    // seconds since 1970
    uint32_t uid, gid;
};

#define ZK_S_ISUID 04000
#define ZK_S_ISGID 02000
#define ZK_S_ISVTX 01000    // sticky: only the owner can delete in this dir

#define ZK_NGROUPS 16

struct zk_cred {
    uint32_t uid, euid, suid;
    uint32_t gid, egid, sgid;
    uint32_t ngroups;
    uint32_t groups[ZK_NGROUPS];
};

struct zk_dirent {
    uint32_t type;
    char name[ZK_NAME_MAX + 1];
};

// fd[i] is the parent's fd that becomes the child's fd i, -1 to leave it closed.
// pgid 0 puts the child in its own group, otherwise it joins pgid
struct zk_spawn {
    int fd[3];
    uint64_t pgid;
};

struct zk_procinfo {
    uint64_t pid, ppid, pgid;
    uint64_t cpu_ms;
    uint32_t state;    // enum thread_state
    uint32_t uid;
    char name[32];
};

struct zk_statfs {
    char fstype[16];
    uint64_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
};

#define ZK_EXIT_KILLED 130

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_NONBLOCK 0x800
#define SOCK_CLOEXEC  0x80000

// same layout as everywhere else: port and address in network byte order
struct zk_sockaddr_in {
    uint16_t family;
    uint16_t port;
    uint32_t addr;
    uint8_t zero[8];
};

// addresses in host byte order
struct zk_netinfo {
    uint8_t mac[6];
    uint8_t up, pad;
    uint32_t ip, mask, gw, dns;
    uint64_t rx_packets, tx_packets, rx_bytes, tx_bytes;
};

// ioctl requests
#define TIOCSFG     0x5401   // arg: pgid that ctrl+c kills, 0 for none
#define TIOCGFG     0x5402   // arg: uint64_t *
#define TIOCGWINSZ  0x5403   // arg: struct zk_winsize *
#define TIOCSWINSZ  0x5404   // arg: const struct zk_winsize *
#define TIOCGMODE   0x5405   // arg: uint32_t *
#define TIOCSMODE   0x5406   // arg: ZK_TTY_* bits
#define BLKRRPART   0x125F   // on a whole disk: read the partition table again
#define FBIOGETINFO 0x4601   // arg: struct zk_fbinfo *
#define FBIOMAP     0x4602   // arg: uint64_t *, gets the address the framebuffer is mapped at

#define ZK_TTY_CANON 1    // line editing, read returns whole lines
#define ZK_TTY_ECHO  2
#define ZK_TTY_ISIG  4    // ctrl+c kills the foreground group
#define ZK_TTY_COOKED (ZK_TTY_CANON | ZK_TTY_ECHO | ZK_TTY_ISIG)

struct zk_winsize {
    uint16_t rows, cols;
    uint16_t xpixels, ypixels;
};

struct zk_fbinfo {
    uint32_t width, height;
    uint32_t pitch;           // bytes per row
    uint32_t bpp;             // always 32
    uint8_t red_shift, green_shift, blue_shift, pad;
};

#define POLLIN   0x01
#define POLLOUT  0x04
#define POLLHUP  0x10
#define POLLNVAL 0x20

struct zk_pollfd {
    int32_t fd;
    int16_t events;
    int16_t revents;
};

// read from /dev/kbd
#define KEYMOD_SHIFT 1
#define KEYMOD_CTRL  2
#define KEYMOD_ALT   4
#define KEYMOD_SUPER 8
#define KEYMOD_CAPS  16

struct zk_key_event {
    uint16_t code;      // scancode set 1, extended keys have 0x80 added
    uint8_t pressed;
    uint8_t ch;         // ascii with modifiers applied, 0 if none
    uint32_t mods;
    uint32_t key;       // ascii or KEY_* special key, 0 if none
};

// read from /dev/mouse
#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4

struct zk_mouse_event {
    int16_t dx, dy;     // dy is positive going down
    int8_t wheel;
    uint8_t buttons;
    uint16_t pad;
};

// special keys in zk_key_event.key and on raw ttys before escape conversion
#define ZK_KEY_UP     0x100
#define ZK_KEY_DOWN   0x101
#define ZK_KEY_LEFT   0x102
#define ZK_KEY_RIGHT  0x103
#define ZK_KEY_HOME   0x104
#define ZK_KEY_END    0x105
#define ZK_KEY_PGUP   0x106
#define ZK_KEY_PGDN   0x107
#define ZK_KEY_INSERT 0x108
#define ZK_KEY_DELETE 0x109
#define ZK_KEY_F1     0x10A
