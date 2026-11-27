#pragma once
// sleeping locks. use these in thread context, spinlocks in irq context
#include "sched/sched.h"

struct mutex {
    bool locked;
    struct thread *owner;
    struct waitq waiters;
};

#define MUTEX_INIT { false, NULL, WAITQ_INIT }

void mutex_lock(struct mutex *m);
bool mutex_trylock(struct mutex *m);
void mutex_unlock(struct mutex *m);

struct semaphore {
    int count;
    struct waitq waiters;
};

#define SEMAPHORE_INIT(n) { (n), WAITQ_INIT }

void sem_down(struct semaphore *s);
void sem_up(struct semaphore *s);
