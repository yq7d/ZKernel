// linux's fd shaped event sources: eventfd, timerfd, signalfd and epoll,
// plus select and the interval timer (alarm, setitimer). event loops like
// glib's, libuv's and the x server's are built on these
#include "proc/lfd.h"
#include "proc/proc.h"
#include "proc/sys.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "fs/vfs.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "sched/ktimer.h"
#include "sched/sched.h"
#include <zk/syscall.h>

#define O_CLOEXEC_L 0x80000
#define LE(n) (-1000 - (n))

struct timespec64 {
    int64_t sec, nsec;
};

static int64_t ts_ms(const struct timespec64 *t)
{
    return t->sec * 1000 + (t->nsec + 999999) / 1000000;
}

static struct timespec64 ms_ts(uint64_t ms)
{
    return (struct timespec64){ (int64_t)(ms / 1000), (int64_t)(ms % 1000) * 1000000 };
}

static int64_t realtime_ms(void)
{
    return rtc_now() * 1000 + (int64_t)(timer_uptime_ms() % 1000);
}

// makes an fd for v, which the file takes over
static int64_t new_fd(struct vnode *v, int flags)
{
    struct file *f = file_new(v, O_RDWR | (flags & O_NONBLOCK));
    vnode_put(v);
    if (!f)
        return ZK_ENOMEM;
    struct proc *p = proc_current();
    int fd = proc_fd_alloc(p, f);
    if (fd < 0) {
        file_put(f);
        return fd;
    }
    if (flags & O_CLOEXEC_L)
        proc_set_cloexec(p, fd, true);
    return fd;
}

static void init_vnode(struct vnode *v, const struct vnode_ops *ops)
{
    *v = (struct vnode){ .type = VNODE_PIPE, .refs = 1, .ino = vfs_next_ino(),
                         .mode = 0600, .ops = ops };
}

static void free_vnode(struct vnode *v)
{
    kfree(v);
}

// blocks until ready() says so. interrupts off around the check
static int64_t wait_until(bool (*ready)(void *), void *arg, struct waitq *wq, bool nonblock)
{
    uint64_t irq = irq_save();
    while (!ready(arg)) {
        if (nonblock) {
            irq_restore(irq);
            return ZK_EAGAIN;
        }
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(wq);
    }
    irq_restore(irq);
    return 0;
}

// eventfd

#define EFD_SEMAPHORE 1

struct efd {
    struct vnode v;
    uint64_t count;
    bool semaphore;
    struct waitq wq;
};

static bool efd_readable(void *a) { return ((struct efd *)a)->count > 0; }

static int64_t efd_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct efd *e = (struct efd *)v;
    if (len < 8)
        return ZK_EINVAL;
    int64_t err = wait_until(efd_readable, e, &e->wq, false);
    if (err)
        return err;
    uint64_t irq = irq_save();
    uint64_t n = e->semaphore ? 1 : e->count;
    e->count -= n;
    irq_restore(irq);
    memcpy(buf, &n, 8);
    waitq_wake_all(&e->wq);
    poll_notify();
    return 8;
}

static int64_t efd_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct efd *e = (struct efd *)v;
    uint64_t n;
    if (len < 8)
        return ZK_EINVAL;
    memcpy(&n, buf, 8);
    if (n == UINT64_MAX)
        return ZK_EINVAL;
    uint64_t irq = irq_save();
    while (e->count > UINT64_MAX - 1 - n) {
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(&e->wq);
    }
    e->count += n;
    irq_restore(irq);
    waitq_wake_all(&e->wq);
    poll_notify();
    return 8;
}

static int efd_poll(struct vnode *v)
{
    struct efd *e = (struct efd *)v;
    return (e->count ? POLLIN : 0) | (e->count < UINT64_MAX - 1 ? POLLOUT : 0);
}

static const struct vnode_ops efd_ops = {
    .read = efd_read, .write = efd_write, .poll = efd_poll, .release = free_vnode,
};

int64_t lfd_eventfd(uint32_t initval, int flags)
{
    struct efd *e = kzalloc(sizeof(*e));
    if (!e)
        return ZK_ENOMEM;
    init_vnode(&e->v, &efd_ops);
    e->count = initval;
    e->semaphore = flags & EFD_SEMAPHORE;
    e->wq = (struct waitq)WAITQ_INIT;
    return new_fd(&e->v, flags);
}

// timerfd

#define TFD_TIMER_ABSTIME 1

struct tfd {
    struct vnode v;
    int clock;
    uint64_t next;        // uptime ms of the next expiry, 0 when disarmed
    uint64_t interval;    // ms
    uint64_t expired;     // expiries not read yet
    struct ktimer timer;
    struct waitq wq;
};

static void tfd_fire(struct ktimer *k)
{
    struct tfd *t = (struct tfd *)((char *)k - __builtin_offsetof(struct tfd, timer));
    uint64_t now = timer_uptime_ms();
    if (!t->next)
        return;
    t->expired++;
    if (t->interval) {
        // catch up on the ones the tick missed
        uint64_t n = (now - t->next) / t->interval;
        t->expired += n;
        t->next += (n + 1) * t->interval;
        ktimer_arm(&t->timer, t->next);
    } else {
        t->next = 0;
    }
    waitq_wake_all(&t->wq);
    poll_notify();
}

static bool tfd_ready(void *a) { return ((struct tfd *)a)->expired > 0; }

static int64_t tfd_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct tfd *t = (struct tfd *)v;
    if (len < 8)
        return ZK_EINVAL;
    int64_t err = wait_until(tfd_ready, t, &t->wq, false);
    if (err)
        return err;
    uint64_t irq = irq_save();
    uint64_t n = t->expired;
    t->expired = 0;
    irq_restore(irq);
    memcpy(buf, &n, 8);
    return 8;
}

static int tfd_poll(struct vnode *v)
{
    return ((struct tfd *)v)->expired ? POLLIN : 0;
}

static void tfd_release(struct vnode *v)
{
    struct tfd *t = (struct tfd *)v;
    ktimer_cancel(&t->timer);
    kfree(t);
}

static const struct vnode_ops tfd_ops = {
    .read = tfd_read, .poll = tfd_poll, .release = tfd_release,
};

int64_t lfd_timerfd_create(int clock, int flags)
{
    // realtime, monotonic, boottime and the alarm variants
    if (clock != 0 && clock != 1 && clock != 7 && clock != 8 && clock != 9)
        return ZK_EINVAL;
    struct tfd *t = kzalloc(sizeof(*t));
    if (!t)
        return ZK_ENOMEM;
    init_vnode(&t->v, &tfd_ops);
    t->clock = clock;
    t->timer.fn = tfd_fire;
    t->wq = (struct waitq)WAITQ_INIT;
    return new_fd(&t->v, flags);
}

struct itimerspec64 {
    struct timespec64 interval, value;
};

static struct tfd *tfd_of(int fd)
{
    struct file *f = proc_fd_get(proc_current(), fd);
    return f && f->v->ops == &tfd_ops ? (struct tfd *)f->v : NULL;
}

static void tfd_current(struct tfd *t, struct itimerspec64 *out)
{
    uint64_t now = timer_uptime_ms();
    out->interval = ms_ts(t->interval);
    out->value = ms_ts(t->next > now ? t->next - now : t->next ? 1 : 0);
}

int64_t lfd_timerfd_settime(int fd, int flags, uint64_t unew, uint64_t uold)
{
    struct tfd *t = tfd_of(fd);
    if (!t)
        return ZK_EBADF;
    struct itimerspec64 n;
    if (copy_in(&n, unew, sizeof n))
        return ZK_EFAULT;
    if (uold) {
        struct itimerspec64 o;
        tfd_current(t, &o);
        if (copy_out(uold, &o, sizeof o))
            return ZK_EFAULT;
    }
    uint64_t irq = irq_save();
    ktimer_cancel(&t->timer);
    t->expired = 0;
    t->interval = (uint64_t)ts_ms(&n.interval);
    int64_t v = ts_ms(&n.value);
    if (n.value.sec == 0 && n.value.nsec == 0) {
        t->next = 0;
    } else {
        uint64_t now = timer_uptime_ms();
        if (flags & TFD_TIMER_ABSTIME) {
            // absolute on its own clock, made relative to uptime
            int64_t base = t->clock == 0 || t->clock == 8 ? realtime_ms() : (int64_t)now;
            v -= base;
        }
        t->next = now + (uint64_t)(v > 0 ? v : 0);
        if (t->next <= now)
            t->next = now;
        ktimer_arm(&t->timer, t->next);
    }
    irq_restore(irq);
    return 0;
}

int64_t lfd_timerfd_gettime(int fd, uint64_t ucur)
{
    struct tfd *t = tfd_of(fd);
    if (!t)
        return ZK_EBADF;
    struct itimerspec64 o;
    tfd_current(t, &o);
    return copy_out(ucur, &o, sizeof o);
}

// signalfd

struct sfd {
    struct vnode v;
    uint64_t mask;
};

struct signalfd_siginfo {
    uint32_t signo;
    int32_t errno_, code;
    uint32_t pid, uid;
    int32_t fd;
    uint32_t tid, band, overrun, trapno;
    int32_t status, int_;
    uint64_t ptr, utime, stime, addr;
    uint16_t addr_lsb;
    uint8_t pad[46];
};

_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd_siginfo");

static uint64_t sfd_pending(struct sfd *s)
{
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    return p ? (p->sig_pending | t->sig_pending) & s->mask : 0;
}

static bool sfd_ready(void *a) { return sfd_pending(a) != 0; }

static int64_t sfd_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct sfd *s = (struct sfd *)v;
    if (len < sizeof(struct signalfd_siginfo))
        return ZK_EINVAL;
    // the signals are blocked, so nothing interrupts the thread: signal_send
    // wakes the poll queue instead
    uint64_t wirq = irq_save();
    while (!sfd_ready(s)) {
        if (proc_killed()) {
            irq_restore(wirq);
            return ZK_EINTR;
        }
        poll_wait(-1);
    }
    irq_restore(wirq);
    size_t done = 0;
    struct thread *t = thread_current();
    struct proc *p = t->proc;
    uint8_t *out = buf;
    while (done + sizeof(struct signalfd_siginfo) <= len) {
        uint64_t irq = irq_save();
        uint64_t bits = sfd_pending(s);
        if (!bits) {
            irq_restore(irq);
            break;
        }
        int sig = __builtin_ctzll(bits) + 1;
        if (t->sig_pending & SIGBIT(sig))
            t->sig_pending &= ~SIGBIT(sig);
        else
            p->sig_pending &= ~SIGBIT(sig);
        struct ksiginfo si = p->siginfo[sig - 1];
        irq_restore(irq);
        struct signalfd_siginfo o;
        memset(&o, 0, sizeof o);
        o.signo = (uint32_t)sig;
        o.code = si.code;
        o.pid = (uint32_t)si.pid;
        o.uid = si.uid;
        o.status = si.status;
        o.addr = si.addr;
        memcpy(out + done, &o, sizeof o);
        done += sizeof o;
    }
    return done ? (int64_t)done : ZK_EINTR;
}

static int sfd_poll(struct vnode *v)
{
    return sfd_pending((struct sfd *)v) ? POLLIN : 0;
}

static const struct vnode_ops sfd_ops = {
    .read = sfd_read, .poll = sfd_poll, .release = free_vnode,
};

int64_t lfd_signalfd(int fd, uint64_t umask, size_t size, int flags)
{
    uint64_t mask;
    if (size != 8)
        return ZK_EINVAL;
    if (copy_in(&mask, umask, 8))
        return ZK_EFAULT;
    mask &= ~SIG_UNBLOCKABLE;
    if (fd != -1) {
        struct file *f = proc_fd_get(proc_current(), fd);
        if (!f || f->v->ops != &sfd_ops)
            return ZK_EINVAL;
        ((struct sfd *)f->v)->mask = mask;
        return fd;
    }
    struct sfd *s = kzalloc(sizeof(*s));
    if (!s)
        return ZK_ENOMEM;
    init_vnode(&s->v, &sfd_ops);
    s->mask = mask;
    return new_fd(&s->v, flags);
}

// epoll. level triggered underneath, which edge triggered users cope with:
// they read until EAGAIN anyway. items are found again by fd and checked
// against the file they were added with, so a closed fd just drops out

#define EPOLLIN      0x001
#define EPOLLOUT     0x004
#define EPOLLERR     0x008
#define EPOLLHUP     0x010
#define EPOLLRDHUP   0x2000
#define EPOLLONESHOT (1U << 30)
#define EPOLLET      (1U << 31)

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

struct eitem {
    int fd;
    struct file *f;     // not a reference, only compared
    uint32_t events;
    uint64_t data;
    bool disabled;      // oneshot that fired
    struct eitem *next;
};

struct ep {
    struct vnode v;
    struct eitem *items;
};

static void ep_release(struct vnode *v)
{
    struct ep *e = (struct ep *)v;
    while (e->items) {
        struct eitem *i = e->items;
        e->items = i->next;
        kfree(i);
    }
    kfree(e);
}

static int ep_poll(struct vnode *v);

static const struct vnode_ops ep_ops = {
    .poll = ep_poll, .release = ep_release,
};

static int ready_bits(struct eitem *i, struct proc *p)
{
    if (i->disabled || proc_fd_get(p, i->fd) != i->f)
        return 0;
    if (i->f->v->ops == &ep_ops)
        return 0;    // no nesting, it would recurse
    int r = file_poll(i->f);
    uint32_t ev = 0;
    if (r & POLLIN)
        ev |= EPOLLIN;
    if (r & POLLOUT)
        ev |= EPOLLOUT;
    if (r & POLLHUP)
        ev |= EPOLLHUP | EPOLLRDHUP;
    if (r & POLLNVAL)
        ev |= EPOLLERR;
    return (int)(ev & (i->events | EPOLLERR | EPOLLHUP));
}

static int ep_poll(struct vnode *v)
{
    struct ep *e = (struct ep *)v;
    struct proc *p = proc_current();
    if (!p)
        return 0;
    for (struct eitem *i = e->items; i; i = i->next)
        if (ready_bits(i, p))
            return POLLIN;
    return 0;
}

int64_t lfd_epoll_create(int flags)
{
    struct ep *e = kzalloc(sizeof(*e));
    if (!e)
        return ZK_ENOMEM;
    init_vnode(&e->v, &ep_ops);
    return new_fd(&e->v, flags);
}

static struct ep *ep_of(int fd)
{
    struct file *f = proc_fd_get(proc_current(), fd);
    return f && f->v->ops == &ep_ops ? (struct ep *)f->v : NULL;
}

int64_t lfd_epoll_ctl(int epfd, int op, int fd, uint64_t uev)
{
    struct ep *e = ep_of(epfd);
    if (!e)
        return ZK_EBADF;
    struct proc *p = proc_current();
    struct file *f = proc_fd_get(p, fd);
    if (!f)
        return ZK_EBADF;
    if (fd == epfd)
        return ZK_EINVAL;
    struct epoll_event ev = { 0, 0 };
    if (op != EPOLL_CTL_DEL && copy_in(&ev, uev, sizeof ev))
        return ZK_EFAULT;

    struct eitem **pp = &e->items;
    while (*pp && !((*pp)->fd == fd && (*pp)->f == f))
        pp = &(*pp)->next;
    // an fd that was closed and reused leaves a stale item: drop it
    for (struct eitem **q = &e->items; *q; ) {
        if ((*q)->fd == fd && (*q)->f != f) {
            struct eitem *dead = *q;
            *q = dead->next;
            if (pp == &dead->next)
                pp = q;
            kfree(dead);
        } else {
            q = &(*q)->next;
        }
    }

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (*pp)
            return ZK_EEXIST;
        struct eitem *i = kzalloc(sizeof(*i));
        if (!i)
            return ZK_ENOMEM;
        *i = (struct eitem){ .fd = fd, .f = f, .events = ev.events, .data = ev.data,
                             .next = e->items };
        e->items = i;
        break;
    }
    case EPOLL_CTL_MOD:
        if (!*pp)
            return ZK_ENOENT;
        (*pp)->events = ev.events;
        (*pp)->data = ev.data;
        (*pp)->disabled = false;
        break;
    case EPOLL_CTL_DEL: {
        if (!*pp)
            return ZK_ENOENT;
        struct eitem *i = *pp;
        *pp = i->next;
        kfree(i);
        break;
    }
    default:
        return ZK_EINVAL;
    }
    poll_notify();
    return 0;
}

int64_t lfd_epoll_wait(int epfd, uint64_t uevents, int max, int64_t timeout,
                       uint64_t usigmask)
{
    struct ep *e = ep_of(epfd);
    if (!e)
        return ZK_EBADF;
    if (max <= 0 || max > 4096)
        return ZK_EINVAL;
    if (!user_ok(uevents, (size_t)max * sizeof(struct epoll_event), true))
        return ZK_EFAULT;

    struct thread *t = thread_current();
    uint64_t oldmask = t->sig_blocked;
    if (usigmask) {
        uint64_t m;
        if (copy_in(&m, usigmask, 8))
            return ZK_EFAULT;
        t->sig_blocked = m & ~SIG_UNBLOCKABLE;
    }

    struct proc *p = proc_current();
    struct epoll_event *out = kmalloc((size_t)max * sizeof(*out));
    if (!out)
        return ZK_ENOMEM;
    uint64_t deadline = timeout > 0 ? timer_uptime_ms() + (uint64_t)timeout : 0;
    int n = 0;
    int64_t r;
    uint64_t irq = irq_save();
    for (;;) {
        n = 0;
        for (struct eitem *i = e->items; i && n < max; i = i->next) {
            int bits = ready_bits(i, p);
            if (!bits)
                continue;
            out[n].events = (uint32_t)bits;
            out[n].data = i->data;
            n++;
            if (i->events & EPOLLONESHOT)
                i->disabled = true;
        }
        if (n || timeout == 0) {
            r = n;
            break;
        }
        if (proc_killed()) {
            r = ZK_EINTR;
            break;
        }
        int64_t left = -1;
        if (timeout > 0) {
            uint64_t now = timer_uptime_ms();
            if (now >= deadline) {
                r = 0;
                break;
            }
            left = (int64_t)(deadline - now);
        }
        poll_wait(left);
    }
    irq_restore(irq);
    if (n)
        copy_out(uevents, out, (size_t)n * sizeof(*out));
    kfree(out);
    if (usigmask) {
        // like sigsuspend, the old mask comes back once a handler has run
        t->sig_saved = oldmask;
        t->sig_restore = true;
    }
    return r;
}

// select, on top of the poll loop

int64_t lfd_select(int nfds, uint64_t urd, uint64_t uwr, uint64_t uex, int64_t timeout_ms)
{
    if (nfds < 0 || nfds > PROC_MAX_FDS)
        nfds = nfds < 0 ? -1 : PROC_MAX_FDS;
    if (nfds < 0)
        return ZK_EINVAL;
    uint64_t rd[PROC_MAX_FDS / 64] = { 0 }, wr[PROC_MAX_FDS / 64] = { 0 }, ex[PROC_MAX_FDS / 64] = { 0 };
    size_t bytes = ((size_t)nfds + 63) / 64 * 8;
    if ((urd && copy_in(rd, urd, bytes)) || (uwr && copy_in(wr, uwr, bytes)) ||
        (uex && copy_in(ex, uex, bytes)))
        return ZK_EFAULT;

    struct zk_pollfd fds[PROC_MAX_FDS];
    size_t n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        bool r = rd[fd / 64] >> (fd % 64) & 1, w = wr[fd / 64] >> (fd % 64) & 1;
        if (!r && !w)
            continue;
        if (!proc_fd_get(proc_current(), fd))
            return ZK_EBADF;
        fds[n].fd = fd;
        fds[n].events = (int16_t)((r ? POLLIN : 0) | (w ? POLLOUT : 0));
        fds[n].revents = 0;
        n++;
    }
    int64_t got = poll_fds(fds, n, timeout_ms);
    if (got < 0)
        return got;
    memset(rd, 0, sizeof rd);
    memset(wr, 0, sizeof wr);
    memset(ex, 0, sizeof ex);
    int64_t count = 0;
    for (size_t i = 0; i < n; i++) {
        int fd = fds[i].fd;
        // hangups count as readable, like linux
        if (fds[i].revents & (POLLIN | POLLHUP) && (fds[i].events & POLLIN)) {
            rd[fd / 64] |= 1UL << (fd % 64);
            count++;
        }
        if (fds[i].revents & POLLOUT) {
            wr[fd / 64] |= 1UL << (fd % 64);
            count++;
        }
    }
    if ((urd && copy_out(urd, rd, bytes)) || (uwr && copy_out(uwr, wr, bytes)) ||
        (uex && copy_out(uex, ex, bytes)))
        return ZK_EFAULT;
    return count;
}

// the real interval timer: alarm and setitimer(ITIMER_REAL)

static void alarm_fire(struct ktimer *k)
{
    struct proc *p = (struct proc *)((char *)k - __builtin_offsetof(struct proc, alarm));
    signal_send(p, SIGALRM, &(struct ksiginfo){ .code = SI_KERNEL });
    if (p->alarm_interval)
        ktimer_arm(k, timer_uptime_ms() + p->alarm_interval);
}

static uint64_t alarm_left(struct proc *p)
{
    uint64_t now = timer_uptime_ms();
    return p->alarm.armed && p->alarm.deadline > now ? p->alarm.deadline - now
         : p->alarm.armed ? 1 : 0;
}

static void alarm_set(struct proc *p, uint64_t value_ms, uint64_t interval_ms)
{
    p->alarm.fn = alarm_fire;
    p->alarm_interval = interval_ms;
    if (value_ms)
        ktimer_arm(&p->alarm, timer_uptime_ms() + value_ms);
    else
        ktimer_cancel(&p->alarm);
}

int64_t lfd_alarm(uint32_t secs)
{
    struct proc *p = proc_current();
    uint64_t left = alarm_left(p);
    alarm_set(p, (uint64_t)secs * 1000, 0);
    return (int64_t)((left + 999) / 1000);
}

struct itimerval {
    int64_t isec, iusec, vsec, vusec;
};

int64_t lfd_setitimer(int which, uint64_t unew, uint64_t uold)
{
    if (which != 0)
        return 0;    // ITIMER_VIRTUAL and ITIMER_PROF: accepted, never fire
    struct proc *p = proc_current();
    if (uold) {
        uint64_t left = alarm_left(p);
        struct itimerval o = {
            (int64_t)(p->alarm_interval / 1000), (int64_t)(p->alarm_interval % 1000) * 1000,
            (int64_t)(left / 1000), (int64_t)(left % 1000) * 1000,
        };
        if (copy_out(uold, &o, sizeof o))
            return ZK_EFAULT;
    }
    if (unew) {
        struct itimerval n;
        if (copy_in(&n, unew, sizeof n))
            return ZK_EFAULT;
        uint64_t v = (uint64_t)(n.vsec * 1000 + (n.vusec + 999) / 1000);
        uint64_t i = (uint64_t)(n.isec * 1000 + (n.iusec + 999) / 1000);
        alarm_set(p, v, i);
    }
    return 0;
}

int64_t lfd_getitimer(int which, uint64_t ucur)
{
    struct itimerval o = { 0, 0, 0, 0 };
    if (which == 0) {
        struct proc *p = proc_current();
        uint64_t left = alarm_left(p);
        o = (struct itimerval){
            (int64_t)(p->alarm_interval / 1000), (int64_t)(p->alarm_interval % 1000) * 1000,
            (int64_t)(left / 1000), (int64_t)(left % 1000) * 1000,
        };
    }
    return copy_out(ucur, &o, sizeof o);
}
