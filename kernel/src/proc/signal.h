#pragma once
// posix signals. numbers, flags and the stack frame follow linux x86_64,
// so linux programs get what they expect. zos programs only ever see the
// default actions
#include <stdbool.h>
#include <stdint.h>

#define NSIG 64

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1   10
#define SIGSEGV   11
#define SIGUSR2   12
#define SIGPIPE   13
#define SIGALRM   14
#define SIGTERM   15
#define SIGSTKFLT 16
#define SIGCHLD   17
#define SIGCONT   18
#define SIGSTOP   19
#define SIGTSTP   20
#define SIGTTIN   21
#define SIGTTOU   22
#define SIGURG    23
#define SIGXCPU   24
#define SIGXFSZ   25
#define SIGVTALRM 26
#define SIGPROF   27
#define SIGWINCH  28
#define SIGIO     29
#define SIGPWR    30
#define SIGSYS    31

#define SIG_DFL 0
#define SIG_IGN 1

#define SA_NOCLDSTOP 0x00000001
#define SA_NOCLDWAIT 0x00000002
#define SA_SIGINFO   0x00000004
#define SA_RESTORER  0x04000000
#define SA_ONSTACK   0x08000000
#define SA_RESTART   0x10000000
#define SA_NODEFER   0x40000000
#define SA_RESETHAND 0x80000000

// si_code
#define SI_USER    0
#define SI_KERNEL  0x80
#define SI_TKILL   (-6)
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define ILL_ILLOPN  2
#define FPE_INTDIV  1
#define BUS_ADRERR  2
#define CLD_EXITED  1
#define CLD_KILLED  2
#define CLD_DUMPED  3

#define SS_ONSTACK 1
#define SS_DISABLE 2

#define SIGBIT(s) (1UL << ((s) - 1))
// these can't be caught, blocked or ignored
#define SIG_UNBLOCKABLE (SIGBIT(SIGKILL) | SIGBIT(SIGSTOP))

// what rt_sigaction takes, minus the restorer being optional
struct ksigaction {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

struct ksiginfo {
    int code;
    int pid;
    uint32_t uid;
    int status;        // SIGCHLD
    uint64_t addr;     // faults
};

struct proc;
struct interrupt_frame;

// queue sig for p. interrupts off or on
void signal_send(struct proc *p, int sig, const struct ksiginfo *info);
// for faults: unblocks sig and resets SIG_IGN, like linux's force_sig
void signal_force(struct proc *p, int sig, const struct ksiginfo *info);
// a signal is waiting that will do something, so blocking calls should
// return EINTR
bool signal_pending(struct proc *p);
// last thing before going back to user mode. syscall is the linux syscall
// number when coming back from one (for SA_RESTART), else -1
void signal_return(struct interrupt_frame *f, int64_t syscall);
// rt_sigreturn, puts the frame back the way it was before the handler
void signal_sigreturn(struct interrupt_frame *f);
// what the default action of sig does
bool signal_default_ignores(int sig);
// fork copies handlers and mask, exec drops handlers
void signal_fork(struct proc *child, const struct proc *parent);
void signal_exec(struct proc *p);
