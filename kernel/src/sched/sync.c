#include "sched/sync.h"
#include "arch/x86_64/cpu.h"
#include "panic.h"

void mutex_lock(struct mutex *m)
{
    uint64_t flags = irq_save();
    if (m->locked && m->owner == thread_current())
        panic("mutex: thread %lu locking a mutex it already holds", m->owner->id);
    while (m->locked)
        waitq_sleep(&m->waiters);
    m->locked = true;
    m->owner = thread_current();
    irq_restore(flags);
}

bool mutex_trylock(struct mutex *m)
{
    uint64_t flags = irq_save();
    bool got = !m->locked;
    if (got) {
        m->locked = true;
        m->owner = thread_current();
    }
    irq_restore(flags);
    return got;
}

void mutex_unlock(struct mutex *m)
{
    uint64_t flags = irq_save();
    if (!m->locked || m->owner != thread_current())
        panic("mutex: unlock by a thread that doesn't hold it");
    m->locked = false;
    m->owner = NULL;
    waitq_wake_one(&m->waiters);
    irq_restore(flags);
}

void sem_down(struct semaphore *s)
{
    uint64_t flags = irq_save();
    while (s->count <= 0)
        waitq_sleep(&s->waiters);
    s->count--;
    irq_restore(flags);
}

void sem_up(struct semaphore *s)
{
    uint64_t flags = irq_save();
    s->count++;
    waitq_wake_one(&s->waiters);
    irq_restore(flags);
}
