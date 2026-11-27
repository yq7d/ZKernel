#pragma once
// single cpu, preemptive round robin. scheduler state is protected by
// disabling interrupts
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum thread_state {
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_SLEEPING,
    THREAD_DEAD,
};

struct thread;

struct waitq {
    struct thread *head, *tail;
};

#define WAITQ_INIT { NULL, NULL }

struct address_space;

struct thread {
    uint64_t rsp;
    uint64_t id;
    char name[32];
    enum thread_state state;

    uint64_t stack_top;
    size_t stack_pages;

    uint64_t wake_tick;
    uint64_t cpu_ticks;

    bool joinable;
    int exit_code;
    struct waitq exit_wq;

    struct address_space *as;    // NULL for kernel threads

    struct thread *next;         // run queue, wait queue or sleep list
    struct thread *all_next;
};

typedef void (*thread_fn)(void *arg);

// adopts the running context as the first thread
void sched_init(uint64_t stack_top, size_t stack_pages);
bool sched_running(void);

struct thread *thread_current(void);
struct thread *thread_create(const char *name, thread_fn fn, void *arg, bool joinable);
__attribute__((noreturn)) void thread_exit(int code);
// waits for a joinable thread and frees it
int thread_join(struct thread *t);
void thread_yield(void);
void thread_sleep_ms(uint64_t ms);

// interrupts must be off. re-check the condition after waking up
void waitq_sleep(struct waitq *wq);
void waitq_wake_one(struct waitq *wq);
void waitq_wake_all(struct waitq *wq);

void sched_tick(void);
void sched_preempt(void);

struct thread_info {
    uint64_t id;
    char name[32];
    enum thread_state state;
    uint64_t cpu_ticks;
    bool user;
};

// newest first
int sched_snapshot(struct thread_info *out, int max);
const char *thread_state_name(enum thread_state s);

// used by the process code to finish a new user thread
struct thread *thread_create_with(const char *name, thread_fn fn, void *arg,
                                  bool joinable, struct address_space *as);
