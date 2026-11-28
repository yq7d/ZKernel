#pragma once
// a process is one user thread plus its address space, fds and cwd.
// pid is the thread id
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zk/syscall.h>
#include "sched/sched.h"
#include "proc/signal.h"
#include "sched/ktimer.h"

#define PROC_MAX_FDS  256
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
    uint64_t cloexec[PROC_MAX_FDS / 64];    // fds closed by exec, one bit each
    char exe[ZK_PATH_MAX];
    int faults_logged;    // crash reports so far, they're capped

    // signals, see signal.h. changed with interrupts off. the mask and
    // the altstack are per thread
    uint64_t sig_pending;      // sent to the process, any thread takes it
    struct ksigaction sigact[NSIG];
    struct ksiginfo siginfo[NSIG];
    int death_sig;             // what it died from, 0 if it exited
    bool core;

    // threads. thread is the main one, its id is the pid. it outlives the
    // others, then tears the process down
    struct thread *threads;    // all of them, through tg_next
    int nthreads;
    struct waitq thread_wq;    // the main thread waits here for the rest
    bool group_exit;           // exit_group or a fatal signal, all threads go
    int group_code;
    bool kill_others;          // exec, every thread but the main one goes
    bool main_gone;            // the main thread called exit, the rest run on

    struct ktimer alarm;       // ITIMER_REAL, SIGALRM when it fires
    uint64_t alarm_interval;   // ms, 0 for one shot
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
// a signal is waiting, blocking calls should give up with EINTR
bool proc_killed(void);
// SIGKILL
int proc_kill(uint64_t pid);
// returns how many it hit
int proc_kill_group(uint64_t pgid);
// pid > 0 one process, 0 the caller's group, -1 everyone the caller may
// signal, < -1 group -pid. sig 0 only checks. checks permissions
int proc_signal(int64_t pid, int sig, int code);
// no permission check, for the tty and the kernel. returns how many it hit
int proc_signal_group(uint64_t pgid, int sig);
// exit_group: ends every thread of the process
__attribute__((noreturn)) void proc_exit(int code);
// exit: ends the calling thread, the process goes with its last thread
__attribute__((noreturn)) void proc_thread_exit(int code);
// the caller's process should be torn down, or at least this thread
bool proc_thread_doomed(void);
#define CLONE_VM             0x100
#define CLONE_FS             0x200
#define CLONE_FILES          0x400
#define CLONE_SIGHAND        0x800
#define CLONE_VFORK          0x4000
#define CLONE_THREAD         0x10000
#define CLONE_SETTLS         0x80000
#define CLONE_PARENT_SETTID  0x100000
#define CLONE_CHILD_CLEARTID 0x200000
#define CLONE_CHILD_SETTID   0x1000000
// clone with CLONE_THREAD. returns the new tid
int64_t proc_clone_thread(struct interrupt_frame *frame, uint64_t flags, uint64_t sp,
                          uint64_t ptid, uint64_t ctid, uint64_t tls);
// a thread by id, and its process. interrupts off
struct thread *proc_find_thread(uint64_t tid);
// the exit code is 128 + sig, like a shell shows it
__attribute__((noreturn)) void proc_exit_signal(int sig, bool core);
// status is the exit code, with the signal it died from in bits 16-23
// and 1 << 24 if it dumped core
int64_t proc_wait(int64_t pid, int *status, int flags);
#define PROC_STATUS_SIG(s)  (((s) >> 16) & 0xff)
#define PROC_STATUS_CORE(s) (((s) >> 24) & 1)
#define PROC_STATUS_CODE(s) ((s) & 0xffff)
void proc_free(struct proc *p);

int proc_fd_alloc(struct proc *p, struct file *f);
// lowest free fd at or above min
int proc_fd_alloc_from(struct proc *p, struct file *f, int min);
void proc_set_cloexec(struct proc *p, int fd, bool on);
bool proc_get_cloexec(struct proc *p, int fd);
struct file *proc_fd_get(struct proc *p, int fd);

int proc_snapshot(struct zk_procinfo *out, int max);
const char *proc_strerror(int err);

void syscall_init(void);
