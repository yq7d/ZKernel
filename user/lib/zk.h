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
int mount(const char *src, const char *target, const char *fstype);
int umount(const char *target);
void sync(void);
int statfs(const char *path, struct zk_statfs *st);
int ftruncate(int fd, uint64_t size);
int reboot(int how);
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
