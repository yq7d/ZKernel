#pragma once
// tiny libc for zkernel user programs
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>
#include "lib/string.h"
#include "lib/printf.h"

__attribute__((noreturn)) void exit(int code);
long write(int fd, const void *buf, size_t len);
long read(int fd, void *buf, size_t len);
long getpid(void);
void sleep_ms(uint64_t ms);
void yield(void);
uint64_t uptime_ms(void);
void *alloc_pages(size_t count);

int open(const char *path, int flags);
int close(int fd);
long seek(int fd, long off, int whence);
int stat(const char *path, struct zk_stat *st);
int fstat(int fd, struct zk_stat *st);
int readdir(int fd, struct zk_dirent *d);
int mkdir(const char *path);
int unlink(const char *path);
int chdir(const char *path);
int getcwd(char *buf, size_t size);
extern char **environ;
char *getenv(const char *name);
int setenv(const char *name, const char *value, int overwrite);
int unsetenv(const char *name);

// argv ends with NULL. opts may be NULL to pass fds 0-2 and start a new group
long spawn(const char *path, char **argv, const struct zk_spawn *opts);
long spawnve(const char *path, char **argv, const struct zk_spawn *opts, char **envp);
long fork(void);
int execve(const char *path, char **argv, char **envp);
int execv(const char *path, char **argv);
int rename(const char *from, const char *to);
long time(void);
int mount(const char *src, const char *target, const char *fstype, const char *opts);
int umount(const char *target);
void sync(void);
int statfs(const char *path, struct zk_statfs *st);
int ftruncate(int fd, uint64_t size);
int reboot(int how);
int ioctl(int fd, unsigned long req, uint64_t arg);
// timeout_ms < 0 waits forever
int poll(struct zk_pollfd *fds, size_t n, long timeout_ms);
int openpty(int fds[2]);
int getcred(struct zk_cred *c);
uint32_t getuid(void);
uint32_t geteuid(void);
uint32_t getgid(void);
int setuid(uint32_t uid);
int setgid(uint32_t gid);
int setgroups(size_t n, const uint32_t *groups);
int chmod(const char *path, uint32_t mode);
int chown(const char *path, uint32_t uid, uint32_t gid);
long getrandom(void *buf, size_t len);

struct sha256 {
    uint32_t h[8];
    uint64_t len;
    uint8_t buf[64];
    size_t used;
};
void sha256_init(struct sha256 *s);
void sha256_update(struct sha256 *s, const void *data, size_t n);
void sha256_final(struct sha256 *s, uint8_t out[32]);
void sha256_hex(const void *data, size_t n, char out[65]);

// /etc/passwd and /etc/group
struct passwd {
    char name[32];
    uint32_t uid, gid;
    char gecos[64];
    char home[ZK_PATH_MAX];
    char shell[ZK_PATH_MAX];
};
int getpwnam(const char *name, struct passwd *pw);
int getpwuid(uint32_t uid, struct passwd *pw);
// the gid of a group, -1 if there's none
long group_id(const char *name);
const char *group_name(uint32_t gid, char *buf, size_t size);
// gids of every group that lists user as a member, plus primary
int user_groups(const char *user, uint32_t primary, uint32_t *out, int max);
// becomes this user: groups, gid, uid, HOME and friends
int become_user(const struct passwd *pw);
// $5z$salt$hash, see scripts/mkpasswd.py
void hash_password(const char *password, const char *salt, char *out, size_t size);
bool check_password(const char *user, const char *password);
// prompts on the tty without echo
long read_password(const char *prompt, char *buf, size_t size);

int symlink(const char *target, const char *path);
long readlink(const char *path, char *buf, size_t size);
int lstat(const char *path, struct zk_stat *st);

// networking. addresses are host byte order except inside zk_sockaddr_in
int socket(int domain, int type);
int connect(int fd, const struct zk_sockaddr_in *a);
// a stream socket connected to a unix socket path
int connect_unix(const char *path);
int bind(int fd, const struct zk_sockaddr_in *a);
long sendto(int fd, const void *buf, size_t len, const struct zk_sockaddr_in *a);
long recvfrom(int fd, void *buf, size_t len, struct zk_sockaddr_in *a);
int shutdown(int fd, int how);
int netinfo(struct zk_netinfo *ni);
// round trip time in ms
long ping(uint32_t ip, uint64_t timeout_ms);
uint16_t net16(uint16_t v);
uint32_t net32(uint32_t v);
bool inet_parse(const char *s, uint32_t *ip);
void inet_format(uint32_t ip, char *out, size_t size);
// a dotted quad, /etc/hosts, or dns through /etc/resolv.conf
int resolve(const char *host, uint32_t *ip);
// connected tcp socket
int tcp_dial(const char *host, uint16_t port);

struct http_progress {
    int status;
    uint64_t total, done;    // total is 0 when the server doesn't say
    void (*fn)(struct http_progress *p);
    void *ctx;
};
// http:// only, follows redirects. writes the body to out
int http_get(const char *url, int out, struct http_progress *progress);
int strncasecmp(const char *a, const char *b, size_t n);
char *strtok_r(char *s, const char *delim, char **save);

// filesystem type ("ext2", "fat") and label of a device
int probe_fs(const char *dev, char *type, size_t tsize, char *label, size_t lsize);
// the device whose filesystem has this label
int find_label(const char *label, char *dev, size_t size, char *type, size_t tsize);

// gzip: appends one member's data to *out (realloc'd), returns bytes of input used
long gunzip_member(const uint8_t *in, size_t len, uint8_t **out, size_t *olen);
void sha1(const void *data, size_t len, uint8_t out[20]);
void base64(const uint8_t *in, size_t len, char *out);
// flags WNOHANG returns 0 when nothing has exited yet
long wait(long pid, int *status, int flags);
int pipe(int fds[2]);
int dup(int fd);
int kill(long pid);
void tty_setfg(long pgid);
int ps(struct zk_procinfo *out, size_t max);

const char *strerror(int err);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int dprintf(int fd, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int vdprintf(int fd, const char *fmt, va_list ap);
int puts(const char *s);
int getchar(void);
// one line from stdin without the newline. -1 at eof with nothing read
long readline(char *buf, size_t size);
// writes everything or fails
long write_all(int fd, const void *buf, size_t len);

int atoi(const char *s);
long strtol(const char *s, char **end, int base);
char *strstr(const char *h, const char *n);
char *strdup(const char *s);
int strcasecmp(const char *a, const char *b);
void qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *));

int isdigit(int c);
int isalpha(int c);
int isalnum(int c);
int isspace(int c);
int isprint(int c);
int isupper(int c);
int islower(int c);
int toupper(int c);
int tolower(int c);

// lines of any length from an fd, without the newline. the line is reused
// by the next call
struct lreader {
    int fd;
    size_t pos, len, cap;
    char *line;
    char buf[1024];
};
struct lreader *lr_open(int fd);
// length, -1 at eof
long lr_getline(struct lreader *r, char **line);
void lr_close(struct lreader *r);

// opens a file named on the command line, "-" is stdin. prints the error
int open_arg(const char *prog, const char *path);
const char *basename_of(const char *path);
// 1.5K, 20M and so on
void format_size(uint64_t bytes, char *out, size_t size);

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t size);
void free(void *p);
