// pseudo terminals: the master end is held by a terminal emulator, the
// slave end looks like a tty to the programs running in it
#include "fs/vfs.h"
#include "arch/x86_64/cpu.h"
#include "drivers/tty.h"
#include "mm/heap.h"
#include "proc/proc.h"

#define OUT_SIZE 8192

struct pty {
    struct tty tty;
    char out[OUT_SIZE];    // slave output waiting for the master
    size_t head, count;
    struct waitq out_readers, out_writers;
    bool master_open, slave_open;
    struct vnode master, slave;
};

// interrupts off. returns how much fit
static size_t out_push(struct pty *p, const char *s, size_t n)
{
    size_t done = 0;
    while (done < n && p->count < OUT_SIZE)
        p->out[(p->head + p->count++) % OUT_SIZE] = s[done++];
    if (done) {
        waitq_wake_all(&p->out_readers);
        poll_notify();
    }
    return done;
}

static void pty_echo(struct tty *t, const char *s, size_t n)
{
    uint64_t flags = irq_save();
    out_push(t->priv, s, n);    // echo drops what doesn't fit
    irq_restore(flags);
}

static void pty_write_out(struct tty *t, const char *s, size_t n)
{
    struct pty *p = t->priv;
    uint64_t flags = irq_save();
    while (n && p->master_open) {
        size_t done = out_push(p, s, n);
        s += done;
        n -= done;
        if (n) {
            if (proc_killed())
                break;
            waitq_sleep(&p->out_writers);
        }
    }
    irq_restore(flags);
}

static const struct tty_ops pty_tty_ops = { pty_write_out, pty_echo };

static void maybe_free(struct pty *p)
{
    if (!p->master_open && !p->slave_open)
        kfree(p);
}

// master

static int64_t m_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pty *p = v->priv;
    char *dst = buf;
    if (len == 0)
        return 0;
    uint64_t flags = irq_save();
    while (p->count == 0) {
        if (!p->slave_open) {
            irq_restore(flags);
            return 0;
        }
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&p->out_readers);
    }
    size_t n = 0;
    while (n < len && p->count) {
        dst[n++] = p->out[p->head];
        p->head = (p->head + 1) % OUT_SIZE;
        p->count--;
    }
    waitq_wake_all(&p->out_writers);
    irq_restore(flags);
    return (int64_t)n;
}

static int64_t m_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pty *p = v->priv;
    const char *s = buf;
    for (size_t i = 0; i < len; i++)
        tty_input(&p->tty, s[i]);
    return (int64_t)len;
}

static int m_poll(struct vnode *v)
{
    struct pty *p = v->priv;
    int r = POLLOUT;
    if (p->count)
        r |= POLLIN;
    if (!p->slave_open)
        r |= POLLIN | POLLHUP;
    return r;
}

static int m_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)f;
    struct pty *p = v->priv;
    return tty_ioctl(&p->tty, req, arg);
}

static void m_release(struct vnode *v)
{
    struct pty *p = v->priv;
    tty_hangup(&p->tty);
    uint64_t flags = irq_save();
    p->master_open = false;
    waitq_wake_all(&p->out_writers);
    maybe_free(p);
    irq_restore(flags);
}

// slave

static int64_t s_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pty *p = v->priv;
    return tty_read(&p->tty, buf, len);
}

static int64_t s_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pty *p = v->priv;
    return tty_write(&p->tty, buf, len);
}

static int s_poll(struct vnode *v)
{
    struct pty *p = v->priv;
    return tty_poll(&p->tty);
}

static int s_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)f;
    struct pty *p = v->priv;
    return tty_ioctl(&p->tty, req, arg);
}

static void s_release(struct vnode *v)
{
    struct pty *p = v->priv;
    uint64_t flags = irq_save();
    p->slave_open = false;
    waitq_wake_all(&p->out_readers);
    poll_notify();
    maybe_free(p);
    irq_restore(flags);
}

static const struct vnode_ops master_ops = {
    .read = m_read, .write = m_write, .poll = m_poll, .ioctl = m_ioctl, .release = m_release,
};

static const struct vnode_ops slave_ops = {
    .read = s_read, .write = s_write, .poll = s_poll, .ioctl = s_ioctl, .release = s_release,
};

struct tty *tty_of(struct vnode *v)
{
    if (v == &tty_vnode)
        return &console_tty;
    if (v->ops == &master_ops || v->ops == &slave_ops)
        return &((struct pty *)v->priv)->tty;
    return NULL;
}

int pty_create(struct file **master, struct file **slave)
{
    struct pty *p = kzalloc(sizeof(*p));
    if (!p)
        return ZK_ENOMEM;
    tty_init(&p->tty, &pty_tty_ops, p);
    p->master_open = p->slave_open = true;
    p->master = (struct vnode){ .type = VNODE_DEV, .refs = 1, .ino = vfs_next_ino(),
                                .ops = &master_ops, .priv = p };
    p->slave = (struct vnode){ .type = VNODE_DEV, .refs = 1, .ino = vfs_next_ino(),
                               .ops = &slave_ops, .priv = p };

    // the files take their own refs, then ours go away
    *master = file_new(&p->master, O_RDWR);
    *slave = file_new(&p->slave, O_RDWR);
    if (!*master || !*slave) {
        if (*master)
            file_put(*master);
        if (*slave)
            file_put(*slave);
        vnode_put(&p->master);
        vnode_put(&p->slave);
        return ZK_ENOMEM;
    }
    vnode_put(&p->master);
    vnode_put(&p->slave);
    return 0;
}
