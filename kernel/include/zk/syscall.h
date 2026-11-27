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
#define SYS_TTY_SETFG   23   // (uint64_t pgid), ctrl+c kills this group, 0 for none
#define SYS_PS          24   // (struct zk_procinfo *out, size_t max) -> count
#define SYS_FORK        25   // () -> child pid, 0 in the child
#define SYS_EXEC        26   // (const char *path, char **argv, char **envp), only returns on error
#define SYS_RENAME      27   // (const char *from, const char *to)
#define SYS_TIME        28   // () -> seconds since 1970, utc
#define SYS_MOUNT       29   // (const char *source, const char *target, const char *fstype)
#define SYS_UMOUNT      30   // (const char *target)
#define SYS_SYNC        31   // ()
#define SYS_STATFS      32   // (const char *path, struct zk_statfs *st)
#define SYS_FTRUNCATE   33   // (int fd, uint64_t size)
#define SYS_REBOOT      34   // (int how), ZK_REBOOT or ZK_POWEROFF. syncs first

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
#define ZK_ERRNO_MAX    26

#define O_RDONLY  0
#define O_WRONLY  1
#define O_RDWR    2
#define O_ACCMODE 3
#define O_CREAT   0x40
#define O_TRUNC   0x200
#define O_APPEND  0x400

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

struct zk_stat {
    uint32_t type;
    uint32_t pad;
    uint64_t size;
    uint64_t ino;
    int64_t mtime;    // seconds since 1970
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
    uint32_t pad;
    char name[32];
};

struct zk_statfs {
    char fstype[16];
    uint64_t block_size;
    uint64_t blocks;
    uint64_t blocks_free;
};

#define ZK_EXIT_KILLED 130
