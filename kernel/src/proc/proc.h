#pragma once
// a process is one user thread plus its address space, fds and cwd.
// pid is the thread id
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>
#include "sched/sched.h"

#define PROC_MAX_FDS  32
#define PROC_MAX_ARGS 64
#define PROC_MAX_ENV  64
#define PROC_MAX_ARG_BYTES (32 * 1024)
#define PROC_MAX_EXEC (16UL << 20)

struct file;

struct proc {
    uint64_t pid, pgid;
    struct thread *thread;
    struct proc *parent;       // NULL once orphaned, or for kernel started ones
    struct proc *list_next;
    struct file *fds[PROC_MAX_FDS];
    char cwd[ZK_PATH_MAX];
    struct waitq child_wq;
    volatile bool killed;
    struct zk_cred cred;
    int abi;                   // ABI_ZOS or ABI_LINUX
    struct vnode *ctty;        // controlling terminal, what /dev/tty opens
    uint32_t cloexec;          // fds closed by exec, one bit each
    char exe[ZK_PATH_MAX];
};

#define ABI_ZOS   0
#define ABI_LINUX 1

struct spawn_opts {
    struct proc *parent;
    struct file *std[3];       // become fds 0-2, NULL leaves them closed
    uint64_t pgid;             // 0 for a new group
    const char *cwd;           // NULL for the parent's, or /
    bool detached;             // no parent and nobody joins it
};

struct interrupt_frame;

// path must be absolute, argv and envp end with NULL (envp may be NULL).
// with no parent the kernel joins the thread, unless detached
struct thread *proc_spawn(const char *path, char **argv, char **envp,
                          const struct spawn_opts *o, int *err);
// frame is the caller's syscall frame. returns the child's pid
int64_t proc_fork(struct interrupt_frame *frame, uint64_t new_rsp, uint64_t child_tid);
// only returns on failure. on success it kfrees release, which may hold the args
int proc_exec(const char *path, char **argv, char **envp, void *release);
// what the kernel starts init and friends with
extern char *proc_default_env[];
// fills std with the console, drop them with proc_put_console
void proc_console_opts(struct spawn_opts *o);
void proc_put_console(struct spawn_opts *o);

struct proc *proc_current(void);
// the caller's credentials, kernel threads are root
const struct zk_cred *cred_current(void);
bool cred_in_group(const struct zk_cred *c, uint32_t gid);
bool proc_killed(void);
int proc_kill(uint64_t pid);
// returns how many it hit
int proc_kill_group(uint64_t pgid);
// call on the way back to user mode
void proc_check_killed(void);
__attribute__((noreturn)) void proc_exit(int code);
int64_t proc_wait(int64_t pid, int *status, int flags);
void proc_free(struct proc *p);

int proc_fd_alloc(struct proc *p, struct file *f);
struct file *proc_fd_get(struct proc *p, int fd);

int proc_snapshot(struct zk_procinfo *out, int max);
const char *proc_strerror(int err);

void syscall_init(void);
