// signal queueing, delivery and rt_sigreturn. the frame pushed for a
// handler is linux's rt_sigframe, so glibc and musl can read the context
#include "proc/signal.h"
#include "proc/proc.h"
#include "proc/sys.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/interrupts.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "sched/sched.h"
#include "fs/vfs.h"

#define USER_CS 0x23
#define USER_SS 0x1B
#define LINUX_EINTR 4
// flags a user may change: cf pf af zf sf tf df of ac
#define USER_RFLAGS 0x40DD5UL
#define RED_ZONE 128

struct l_sigcontext {
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rdi, rsi, rbp, rbx, rdx, rax, rcx, rsp, rip, eflags;
    uint16_t cs, gs, fs, ss;
    uint64_t err, trapno, oldmask, cr2;
    uint64_t fpstate;
    uint64_t reserved[8];
};

struct l_stack {
    uint64_t sp;
    int32_t flags;
    int32_t pad;
    uint64_t size;
};

struct l_ucontext {
    uint64_t flags;
    uint64_t link;
    struct l_stack stack;
    struct l_sigcontext mc;
    uint64_t sigmask;
};

struct l_siginfo {
    int32_t signo, errno_, code, pad;
    union {
        struct { int32_t pid; uint32_t uid; int32_t status; } kill;
        uint64_t addr;
        uint8_t raw[112];
    };
};

struct l_sigframe {
    uint64_t pretcode;
    struct l_ucontext uc;
    struct l_siginfo info;
};

_Static_assert(sizeof(struct l_sigcontext) == 256, "sigcontext");
_Static_assert(sizeof(struct l_siginfo) == 128, "siginfo");
_Static_assert(__builtin_offsetof(struct l_ucontext, mc) == 40, "ucontext");

bool signal_default_ignores(int sig)
{
    // no job control yet, so the stop signals and SIGCONT do nothing
    switch (sig) {
    case SIGCHLD: case SIGCONT: case SIGURG: case SIGWINCH:
    case SIGSTOP: case SIGTSTP: case SIGTTIN: case SIGTTOU:
        return true;
    }
    return false;
}

static bool dumps_core(int sig)
{
    switch (sig) {
    case SIGQUIT: case SIGILL: case SIGTRAP: case SIGABRT: case SIGBUS:
    case SIGFPE: case SIGSEGV: case SIGXCPU: case SIGXFSZ: case SIGSYS:
        return true;
    }
    return false;
}

static bool ignored(struct proc *p, int sig)
{
    uint64_t h = p->sigact[sig - 1].handler;
    return h == SIG_IGN || (h == SIG_DFL && signal_default_ignores(sig));
}

static void interrupt(struct thread *t)
{
    if (t != thread_current())
        thread_interrupt(t);
}

static void interrupt_all(struct proc *p)
{
    for (struct thread *t = p->threads; t; t = t->tg_next)
        interrupt(t);
}

static void save_info(struct proc *p, int sig, const struct ksiginfo *info)
{
    if (info)
        p->siginfo[sig - 1] = *info;
    else
        memset(&p->siginfo[sig - 1], 0, sizeof p->siginfo[0]);
}

void signal_send(struct proc *p, int sig, const struct ksiginfo *info)
{
    if (sig < 1 || sig > NSIG)
        return;
    uint64_t flags = irq_save();
    if (sig == SIGKILL) {
        p->killed = true;
        interrupt_all(p);
    } else if (!ignored(p, sig)) {
        // linux drops ignored signals right away, even blocked ones
        p->sig_pending |= SIGBIT(sig);
        save_info(p, sig, info);
        // wake one thread that will take it, the main one first like linux
        struct thread *m = p->thread;
        if (m && !p->main_gone && !(m->sig_blocked & SIGBIT(sig))) {
            interrupt(m);
        } else {
            for (struct thread *t = p->threads; t; t = t->tg_next) {
                if (t == m || (t->sig_blocked & SIGBIT(sig)))
                    continue;
                interrupt(t);
                break;
            }
        }
    }
    irq_restore(flags);
    poll_notify();    // signalfd readers
}

void signal_send_thread(struct thread *t, int sig, const struct ksiginfo *info)
{
    struct proc *p = t->proc;
    if (sig < 1 || sig > NSIG || !p)
        return;
    if (sig == SIGKILL) {
        signal_send(p, sig, info);
        return;
    }
    uint64_t flags = irq_save();
    if (!ignored(p, sig)) {
        t->sig_pending |= SIGBIT(sig);
        save_info(p, sig, info);
        if (!(t->sig_blocked & SIGBIT(sig)))
            interrupt(t);
    }
    irq_restore(flags);
    poll_notify();
}

void signal_force(struct thread *t, int sig, const struct ksiginfo *info)
{
    struct proc *p = t->proc;
    uint64_t flags = irq_save();
    struct ksigaction *a = &p->sigact[sig - 1];
    if (a->handler == SIG_IGN || (t->sig_blocked & SIGBIT(sig)))
        a->handler = SIG_DFL;
    t->sig_blocked &= ~SIGBIT(sig);
    t->sig_pending |= SIGBIT(sig);
    save_info(p, sig, info);
    irq_restore(flags);
}

bool signal_pending(struct thread *t)
{
    struct proc *p = t->proc;
    if (!p)
        return false;
    if (p->killed || p->group_exit || (p->kill_others && t != p->thread))
        return true;
    return (p->sig_pending | t->sig_pending) & ~t->sig_blocked;
}

void signal_fork(struct proc *child, struct thread *ct, const struct proc *parent,
                 const struct thread *pt)
{
    memcpy(child->sigact, parent->sigact, sizeof child->sigact);
    child->sig_pending = 0;
    ct->sig_pending = 0;
    ct->sig_blocked = pt->sig_blocked;
    ct->altstack_sp = pt->altstack_sp;
    ct->altstack_size = pt->altstack_size;
    ct->altstack_flags = pt->altstack_flags;
}

void signal_exec(struct proc *p, struct thread *t)
{
    // handlers point into the old image, ignored stays ignored
    for (int i = 0; i < NSIG; i++) {
        if (p->sigact[i].handler != SIG_IGN)
            p->sigact[i].handler = SIG_DFL;
        p->sigact[i].flags = 0;
        p->sigact[i].mask = 0;
        p->sigact[i].restorer = 0;
    }
    t->altstack_sp = 0;
    t->altstack_size = 0;
    t->altstack_flags = SS_DISABLE;
}

__attribute__((noreturn)) static void die(int sig)
{
    sti();
    proc_exit_signal(sig, dumps_core(sig));
}

// syscalls that come back with EINTR even with SA_RESTART, like on linux
static bool restartable(int64_t nr)
{
    switch (nr) {
    case 7: case 23: case 34: case 35: case 130: case 230: case 232:
    case 270: case 271: case 281:
        return false;
    }
    return true;
}

static bool on_altstack(struct thread *p, uint64_t sp)
{
    return p->altstack_size && sp > p->altstack_sp &&
           sp <= p->altstack_sp + p->altstack_size;
}

// false if the user stack is unusable
static bool push_frame(struct thread *p, struct interrupt_frame *f, int sig,
                       const struct ksigaction *a, const struct ksiginfo *si)
{
    uint64_t sp = f->rsp;
    if ((a->flags & SA_ONSTACK) && !(p->altstack_flags & SS_DISABLE) &&
        p->altstack_size && !on_altstack(p, sp))
        sp = p->altstack_sp + p->altstack_size;
    else
        sp -= RED_ZONE;

    // the fpu registers are still the user's, the kernel never uses them
    struct fpu_state fx;
    fpu_save(&fx);
    sp = (sp - sizeof fx) & ~63UL;
    uint64_t fp_addr = sp;
    if (copy_out(fp_addr, &fx, sizeof fx))
        return false;

    struct l_sigframe fr;
    memset(&fr, 0, sizeof fr);
    sp -= sizeof fr;
    sp = (sp & ~15UL) - 8;    // like after a call: rsp + 8 is 16 aligned

    fr.pretcode = a->restorer;
    fr.uc.stack.sp = p->altstack_sp;
    fr.uc.stack.size = p->altstack_size;
    fr.uc.stack.flags = p->altstack_flags | (on_altstack(p, f->rsp) ? SS_ONSTACK : 0);
    struct l_sigcontext *mc = &fr.uc.mc;
    mc->r8 = f->r8;   mc->r9 = f->r9;   mc->r10 = f->r10; mc->r11 = f->r11;
    mc->r12 = f->r12; mc->r13 = f->r13; mc->r14 = f->r14; mc->r15 = f->r15;
    mc->rdi = f->rdi; mc->rsi = f->rsi; mc->rbp = f->rbp; mc->rbx = f->rbx;
    mc->rdx = f->rdx; mc->rax = f->rax; mc->rcx = f->rcx; mc->rsp = f->rsp;
    mc->rip = f->rip; mc->eflags = f->rflags;
    mc->cs = USER_CS; mc->ss = USER_SS;
    mc->trapno = f->vector < 32 ? f->vector : 0;
    mc->err = f->vector < 32 ? f->error_code : 0;
    mc->cr2 = si->addr;
    uint64_t old = p->sig_restore ? p->sig_saved : p->sig_blocked;
    mc->oldmask = old;
    mc->fpstate = fp_addr;
    fr.uc.sigmask = old;

    fr.info.signo = sig;
    fr.info.code = si->code;
    if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL || sig == SIGFPE) {
        fr.info.addr = si->addr;
    } else {
        fr.info.kill.pid = si->pid;
        fr.info.kill.uid = si->uid;
        fr.info.kill.status = si->status;
    }

    if (copy_out(sp, &fr, sizeof fr))
        return false;

    f->rsp = sp;
    f->rip = a->handler;
    f->rdi = (uint64_t)sig;
    f->rsi = sp + __builtin_offsetof(struct l_sigframe, info);
    f->rdx = sp + __builtin_offsetof(struct l_sigframe, uc);
    f->rax = 0;
    f->rflags &= ~(1UL << 10 | 1UL << 8);    // df and tf off for the handler
    f->cs = USER_CS;
    f->ss = USER_SS;
    return true;
}

void signal_return(struct interrupt_frame *f, int64_t syscall)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    if (!p)
        return;
    bool eintr = syscall >= 0 && (int64_t)f->rax == -LINUX_EINTR;

    for (;;) {
        if (p->killed)
            die(SIGKILL);
        // another thread is ending the process, or exec wants us gone
        if (p->group_exit || (p->kill_others && t != p->thread)) {
            sti();
            proc_thread_exit(0);
        }
        uint64_t flags = irq_save();
        uint64_t bits = (t->sig_pending | p->sig_pending) & ~t->sig_blocked;
        if (!bits) {
            irq_restore(flags);
            break;
        }
        int sig = __builtin_ctzll(bits) + 1;
        if (t->sig_pending & SIGBIT(sig))
            t->sig_pending &= ~SIGBIT(sig);
        else
            p->sig_pending &= ~SIGBIT(sig);
        struct ksiginfo si = p->siginfo[sig - 1];
        struct ksigaction a = p->sigact[sig - 1];
        irq_restore(flags);

        if (a.handler == SIG_IGN)
            continue;
        if (a.handler == SIG_DFL) {
            if (signal_default_ignores(sig))
                continue;
            die(sig);
        }
        // no restorer means nowhere for the handler to return to
        if (p->abi != ABI_LINUX || !(a.flags & SA_RESTORER))
            die(sig);

        if (eintr && (a.flags & SA_RESTART) && restartable(syscall)) {
            f->rax = (uint64_t)syscall;
            f->rip -= 2;    // back onto the syscall instruction
        }
        if (!push_frame(t, f, sig, &a, &si))
            die(SIGSEGV);

        flags = irq_save();
        t->sig_restore = false;
        if (a.flags & SA_RESETHAND)
            p->sigact[sig - 1].handler = SIG_DFL;
        t->sig_blocked |= a.mask & ~SIG_UNBLOCKABLE;
        if (!(a.flags & SA_NODEFER))
            t->sig_blocked |= SIGBIT(sig);
        irq_restore(flags);
        // one handler at a time, the rest wait for rt_sigreturn
        return;
    }

    if (t->sig_restore) {
        uint64_t flags = irq_save();
        t->sig_blocked = t->sig_saved;
        t->sig_restore = false;
        irq_restore(flags);
    }
    // interrupted, but nothing ran: go back in as if nothing happened
    if (eintr && restartable(syscall)) {
        f->rax = (uint64_t)syscall;
        f->rip -= 2;
    }
}

void signal_sigreturn(struct interrupt_frame *f)
{
    struct thread *p = thread_current();
    // the handler's ret popped pretcode, so the ucontext starts at rsp
    uint64_t uc_addr = f->rsp;
    struct l_ucontext uc;
    if (copy_in(&uc, uc_addr, sizeof uc))
        die(SIGSEGV);

    struct l_sigcontext *mc = &uc.mc;
    if (mc->rip >= USER_LIMIT || mc->rsp >= USER_LIMIT)
        die(SIGSEGV);
    if (mc->fpstate) {
        struct fpu_state fx;
        if (copy_in(&fx, mc->fpstate, sizeof fx))
            die(SIGSEGV);
        // reserved mxcsr bits would #gp on fxrstor
        uint32_t *mxcsr = (uint32_t *)&fx.data[24];
        *mxcsr &= 0xFFBF;
        fpu_restore(&fx);
    }

    f->r8 = mc->r8;   f->r9 = mc->r9;   f->r10 = mc->r10; f->r11 = mc->r11;
    f->r12 = mc->r12; f->r13 = mc->r13; f->r14 = mc->r14; f->r15 = mc->r15;
    f->rdi = mc->rdi; f->rsi = mc->rsi; f->rbp = mc->rbp; f->rbx = mc->rbx;
    f->rdx = mc->rdx; f->rax = mc->rax; f->rcx = mc->rcx; f->rsp = mc->rsp;
    f->rip = mc->rip;
    f->rflags = (f->rflags & ~USER_RFLAGS) | (mc->eflags & USER_RFLAGS);
    f->cs = USER_CS;
    f->ss = USER_SS;

    uint64_t flags = irq_save();
    p->sig_blocked = uc.sigmask & ~SIG_UNBLOCKABLE;
    irq_restore(flags);
}
