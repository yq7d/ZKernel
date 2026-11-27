// futexes. a waiter lives on its own kernel stack, linked into a bucket
// for its address. waking unlinks it, so a waiter that's still linked
// after sleeping wasn't woken
#include "proc/futex.h"
#include "proc/proc.h"
#include "proc/sys.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pit.h"
#include "sched/sched.h"
#include <zk/syscall.h>

#define BUCKETS 64

struct waiter {
    struct address_space *as;
    uint64_t addr;
    uint32_t bitset;
    bool woken;
    struct waitq wq;
    struct waiter *next;
};

static struct waiter *buckets[BUCKETS];

static struct waiter **bucket(struct address_space *as, uint64_t addr)
{
    uint64_t h = (addr >> 2) ^ ((uint64_t)as >> 6);
    h ^= h >> 17;
    return &buckets[h % BUCKETS];
}

static void unlink(struct waiter *w)
{
    for (struct waiter **pp = bucket(w->as, w->addr); *pp; pp = &(*pp)->next) {
        if (*pp == w) {
            *pp = w->next;
            return;
        }
    }
}

int64_t futex_wait(uint64_t uaddr, uint32_t val, int64_t timeout_ms, uint32_t bitset)
{
    if (uaddr & 3)
        return ZK_EINVAL;
    if (!bitset)
        return ZK_EINVAL;
    struct waiter w = {
        .as = thread_current()->as, .addr = uaddr, .bitset = bitset, .wq = WAITQ_INIT,
    };
    uint64_t deadline = timeout_ms >= 0 ? timer_uptime_ms() + (uint64_t)timeout_ms : 0;

    // checking the value and queueing happen with interrupts off, so a
    // wake in between can't be missed
    uint64_t flags = irq_save();
    uint32_t cur;
    if (copy_in(&cur, uaddr, 4)) {
        irq_restore(flags);
        return ZK_EFAULT;
    }
    if (cur != val) {
        irq_restore(flags);
        return ZK_EAGAIN;
    }
    struct waiter **b = bucket(w.as, uaddr);
    w.next = *b;
    *b = &w;

    int64_t r = 0;
    while (!w.woken) {
        if (proc_killed()) {
            r = ZK_EINTR;
            break;
        }
        if (timeout_ms >= 0) {
            uint64_t now = timer_uptime_ms();
            if (now >= deadline) {
                r = ZK_ETIMEDOUT;
                break;
            }
            waitq_sleep_timeout(&w.wq, deadline - now);
        } else {
            waitq_sleep(&w.wq);
        }
    }
    if (!w.woken)
        unlink(&w);
    irq_restore(flags);
    return r;
}

int64_t futex_wake(struct address_space *as, uint64_t uaddr, int n, uint32_t bitset)
{
    int woken = 0;
    uint64_t flags = irq_save();
    struct waiter **pp = bucket(as, uaddr);
    while (*pp && woken < n) {
        struct waiter *w = *pp;
        if (w->as == as && w->addr == uaddr && (w->bitset & bitset)) {
            *pp = w->next;
            w->woken = true;
            waitq_wake_one(&w->wq);
            woken++;
        } else {
            pp = &w->next;
        }
    }
    irq_restore(flags);
    return woken;
}

int64_t futex_requeue(uint64_t uaddr, int n, uint64_t uaddr2, int n2)
{
    struct address_space *as = thread_current()->as;
    uint64_t flags = irq_save();
    int64_t done = futex_wake(as, uaddr, n, FUTEX_BITSET_ANY);
    struct waiter **pp = bucket(as, uaddr);
    int moved = 0;
    while (*pp && moved < n2) {
        struct waiter *w = *pp;
        if (w->as == as && w->addr == uaddr) {
            *pp = w->next;
            w->addr = uaddr2;
            struct waiter **b = bucket(as, uaddr2);
            w->next = *b;
            *b = w;
            moved++;
        } else {
            pp = &w->next;
        }
    }
    irq_restore(flags);
    return done + moved;
}
