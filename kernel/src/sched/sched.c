#include "sched/sched.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "drivers/pit.h"
#include "sched/ktimer.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/vmm.h"
#include "panic.h"
#include "proc/proc.h"

#define THREAD_STACK_PAGES 16
#define TIME_SLICE         10    // ticks

extern void switch_context(uint64_t *old_rsp, uint64_t new_rsp);
extern void thread_trampoline(void);

static struct thread *current;
static struct thread *idle_thread;
static struct thread *all_threads;
static struct waitq ready = WAITQ_INIT;
static struct thread *sleepers;
static struct thread *zombies;

static bool running;
static volatile bool need_resched;
static int slice_left;
static uint64_t next_id;

static void enqueue(struct waitq *q, struct thread *t)
{
    t->next = NULL;
    if (q->tail)
        q->tail->next = t;
    else
        q->head = t;
    q->tail = t;
}

static struct thread *dequeue(struct waitq *q)
{
    struct thread *t = q->head;
    if (t) {
        q->head = t->next;
        if (!q->head)
            q->tail = NULL;
        t->next = NULL;
    }
    return t;
}

static void unlink_sleeper(struct thread *t)
{
    for (struct thread **p = &sleepers; *p; p = &(*p)->sleep_next) {
        if (*p == t) {
            *p = t->sleep_next;
            return;
        }
    }
}

static void unlink_waiter(struct waitq *q, struct thread *t)
{
    struct thread *prev = NULL;
    for (struct thread *c = q->head; c; prev = c, c = c->next) {
        if (c != t)
            continue;
        if (prev)
            prev->next = c->next;
        else
            q->head = c->next;
        if (q->tail == c)
            q->tail = prev;
        c->next = NULL;
        return;
    }
}

static void make_ready(struct thread *t)
{
    // woken through the queue before the timeout hit
    if (t->timed_wq) {
        unlink_sleeper(t);
        t->timed_wq = NULL;
        t->timed_out = false;
    }
    t->blocked_on = NULL;
    t->state = THREAD_READY;
    enqueue(&ready, t);
    if (current == idle_thread)
        need_resched = true;
}

// interrupts must be off
static void schedule(void)
{
    struct thread *prev = current;
    need_resched = false;

    if (prev->state == THREAD_RUNNING) {
        if (prev == idle_thread)
            prev->state = THREAD_READY;
        else
            make_ready(prev);
    }

    struct thread *next = dequeue(&ready);
    if (!next)
        next = idle_thread;

    slice_left = TIME_SLICE;
    if (next == prev) {
        prev->state = THREAD_RUNNING;
        return;
    }

    next->state = THREAD_RUNNING;
    current = next;
    tss_set_kernel_stack(next->stack_top);
    vmm_activate(next->as);
    fpu_save(prev->fpu);
    fpu_restore(next->fpu);
    if (prev->fs_base != next->fs_base)
        wrmsr(MSR_FS_BASE, next->fs_base);
    switch_context(&prev->rsp, next->rsp);
}

static void free_thread(struct thread *t)
{
    if (t->proc && !t->extra)
        proc_free(t->proc);
    if (t->as && !t->extra)
        vmm_destroy_space(t->as);
    vmm_free_stack(t->stack_top, t->stack_pages);
    kfree(t->fpu);
    kfree(t);
}

static void unlink_all(struct thread *t)
{
    for (struct thread **p = &all_threads; *p; p = &(*p)->all_next) {
        if (*p == t) {
            *p = t->all_next;
            return;
        }
    }
}

static void reap(void)
{
    uint64_t flags = irq_save();
    struct thread *list = zombies;
    zombies = NULL;
    for (struct thread *t = list; t; t = t->next)
        unlink_all(t);
    irq_restore(flags);

    while (list) {
        struct thread *t = list;
        list = t->next;
        free_thread(t);
    }
}

static void idle_loop(void *arg)
{
    (void)arg;
    for (;;) {
        reap();
        __asm__ volatile ("sti; hlt");
    }
}

static struct thread *alloc_thread(const char *name)
{
    struct thread *t = kzalloc(sizeof(*t));
    if (!t)
        return NULL;
    t->fpu = kmalloc(sizeof(*t->fpu));
    if (!t->fpu) {
        kfree(t);
        return NULL;
    }
    fpu_init_state(t->fpu);
    strlcpy(t->name, name, sizeof t->name);
    t->id = __atomic_fetch_add(&next_id, 1, __ATOMIC_RELAXED);
    return t;
}

static struct thread *spawn(const char *name, thread_fn fn, void *arg,
                            bool joinable, struct address_space *as,
                            struct proc *proc, bool runnable)
{
    reap();

    struct thread *t = alloc_thread(name);
    if (!t)
        return NULL;
    t->stack_pages = THREAD_STACK_PAGES;
    t->stack_top = vmm_alloc_stack(THREAD_STACK_PAGES);
    if (!t->stack_top) {
        kfree(t->fpu);
        kfree(t);
        return NULL;
    }
    t->joinable = joinable;
    t->as = as;
    t->proc = proc;

    // frame for switch_context to pop: r15 r14 r13 r12 rbx rbp, then ret
    uint64_t *sp = (uint64_t *)t->stack_top;
    *--sp = 0;
    *--sp = (uint64_t)thread_trampoline;
    *--sp = 0;                  // rbp
    *--sp = 0;                  // rbx
    *--sp = (uint64_t)fn;       // r12
    *--sp = (uint64_t)arg;      // r13
    *--sp = 0;                  // r14
    *--sp = 0;                  // r15
    t->rsp = (uint64_t)sp;

    uint64_t flags = irq_save();
    t->all_next = all_threads;
    all_threads = t;
    if (runnable)
        make_ready(t);
    else
        t->state = THREAD_READY;
    irq_restore(flags);
    return t;
}

struct thread *thread_create_with(const char *name, thread_fn fn, void *arg,
                                  bool joinable, struct address_space *as,
                                  struct proc *proc)
{
    return spawn(name, fn, arg, joinable, as, proc, true);
}

struct thread *thread_create(const char *name, thread_fn fn, void *arg, bool joinable)
{
    return thread_create_with(name, fn, arg, joinable, NULL, NULL);
}

void sched_init(uint64_t stack_top, size_t stack_pages)
{
    struct thread *boot = alloc_thread("kmain");
    if (!boot)
        panic("sched: out of memory");
    boot->state = THREAD_RUNNING;
    boot->stack_top = stack_top;
    boot->stack_pages = stack_pages;
    boot->all_next = all_threads;
    all_threads = boot;
    current = boot;

    // never on the run queue, schedule() falls back to it
    idle_thread = spawn("idle", idle_loop, NULL, false, NULL, NULL, false);
    if (!idle_thread)
        panic("sched: can't create the idle thread");

    slice_left = TIME_SLICE;
    running = true;
}

bool sched_running(void)
{
    return running;
}

struct thread *thread_current(void)
{
    return current;
}

void thread_exit(int code)
{
    cli();
    struct thread *t = current;
    t->exit_code = code;
    t->state = THREAD_DEAD;

    if (t->as) {
        // can't free the tables we're running on
        vmm_activate(NULL);
    }

    if (t->joinable) {
        waitq_wake_all(&t->exit_wq);
    } else {
        t->next = zombies;
        zombies = t;
    }
    schedule();
    panic("sched: dead thread %lu was scheduled", t->id);
}

int thread_join(struct thread *t)
{
    if (!t->joinable)
        panic("sched: joining detached thread %lu", t->id);

    uint64_t flags = irq_save();
    while (t->state != THREAD_DEAD)
        waitq_sleep(&t->exit_wq);
    unlink_all(t);
    irq_restore(flags);

    int code = t->exit_code;
    free_thread(t);
    return code;
}

void thread_detach(struct thread *t)
{
    if (!t->joinable)
        return;
    t->joinable = false;
    if (t->state == THREAD_DEAD) {
        t->next = zombies;
        zombies = t;
    }
}

void thread_yield(void)
{
    uint64_t flags = irq_save();
    schedule();
    irq_restore(flags);
}

void thread_sleep_ms(uint64_t ms)
{
    if (ms == 0) {
        thread_yield();
        return;
    }
    uint64_t ticks = (ms * TIMER_HZ + 999) / 1000;

    uint64_t flags = irq_save();
    current->wake_tick = timer_ticks() + ticks;
    current->state = THREAD_SLEEPING;
    current->sleep_next = sleepers;
    sleepers = current;
    schedule();
    irq_restore(flags);
}

void waitq_sleep(struct waitq *wq)
{
    current->state = THREAD_BLOCKED;
    current->blocked_on = wq;
    enqueue(wq, current);
    schedule();
}

bool waitq_sleep_timeout(struct waitq *wq, uint64_t ms)
{
    uint64_t ticks = (ms * TIMER_HZ + 999) / 1000;
    current->state = THREAD_BLOCKED;
    current->blocked_on = wq;
    enqueue(wq, current);
    current->timed_wq = wq;
    current->timed_out = false;
    current->wake_tick = timer_ticks() + (ticks ? ticks : 1);
    current->sleep_next = sleepers;
    sleepers = current;
    schedule();
    return !current->timed_out;
}

void waitq_wake_one(struct waitq *wq)
{
    uint64_t flags = irq_save();
    struct thread *t = dequeue(wq);
    if (t)
        make_ready(t);
    irq_restore(flags);
}

void waitq_wake_all(struct waitq *wq)
{
    uint64_t flags = irq_save();
    struct thread *t;
    while ((t = dequeue(wq)))
        make_ready(t);
    irq_restore(flags);
}

void thread_interrupt(struct thread *t)
{
    if (t->state == THREAD_SLEEPING) {
        unlink_sleeper(t);
        make_ready(t);
    } else if (t->state == THREAD_BLOCKED && t->blocked_on) {
        unlink_waiter(t->blocked_on, t);
        make_ready(t);
    }
}

// timer irq, interrupts are off
void sched_tick(void)
{
    if (!running)
        return;

    current->cpu_ticks++;
    ktimer_tick(timer_uptime_ms());

    uint64_t now = timer_ticks();
    struct thread **p = &sleepers;
    while (*p) {
        struct thread *t = *p;
        if (now >= t->wake_tick) {
            *p = t->sleep_next;
            if (t->timed_wq) {
                unlink_waiter(t->timed_wq, t);
                t->timed_wq = NULL;
                t->timed_out = true;
            }
            make_ready(t);
        } else {
            p = &t->sleep_next;
        }
    }

    if (current == idle_thread) {
        if (ready.head)
            need_resched = true;
    } else if (--slice_left <= 0) {
        need_resched = true;
    }
}

void sched_resched(void)
{
    need_resched = true;
}

// end of an irq, after the eoi
void sched_preempt(void)
{
    if (running && need_resched)
        schedule();
}

int sched_snapshot(struct thread_info *out, int max)
{
    int n = 0;
    uint64_t flags = irq_save();
    for (struct thread *t = all_threads; t && n < max; t = t->all_next) {
        out[n].id = t->id;
        out[n].state = t->state;
        out[n].cpu_ticks = t->cpu_ticks;
        out[n].user = t->as != NULL;
        strlcpy(out[n].name, t->name, sizeof out[n].name);
        n++;
    }
    irq_restore(flags);
    return n;
}

const char *thread_state_name(enum thread_state s)
{
    switch (s) {
    case THREAD_READY:    return "ready";
    case THREAD_RUNNING:  return "running";
    case THREAD_BLOCKED:  return "blocked";
    case THREAD_SLEEPING: return "sleeping";
    case THREAD_DEAD:     return "dead";
    }
    return "?";
}
