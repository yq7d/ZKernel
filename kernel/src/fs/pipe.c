#include "fs/vfs.h"
#include "arch/x86_64/cpu.h"
#include "mm/heap.h"
#include "proc/proc.h"
#include "sched/sched.h"

#define PIPE_SIZE 4096

struct pipe {
    uint8_t buf[PIPE_SIZE];
    size_t head, count;
    bool rd_open, wr_open;
    struct waitq readers, writers;
    struct vnode rd, wr;
};

static int pipe_poll_rd(struct vnode *v);
static int pipe_poll_wr(struct vnode *v);

static int64_t pipe_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pipe *p = v->priv;
    uint8_t *dst = buf;
    if (len == 0)
        return 0;

    uint64_t flags = irq_save();
    while (p->count == 0 && p->wr_open) {
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&p->readers);
    }
    size_t n = 0;
    while (n < len && p->count) {
        dst[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_SIZE;
        p->count--;
    }
    waitq_wake_all(&p->writers);
    poll_notify();
    irq_restore(flags);
    return (int64_t)n;
}

static int64_t pipe_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)off;
    struct pipe *p = v->priv;
    const uint8_t *src = buf;
    size_t n = 0;

    uint64_t flags = irq_save();
    while (n < len) {
        if (!p->rd_open) {
            irq_restore(flags);
            return n ? (int64_t)n : ZK_EPIPE;
        }
        if (p->count == PIPE_SIZE) {
            if (proc_killed())
                break;
            waitq_wake_all(&p->readers);
            waitq_sleep(&p->writers);
            continue;
        }
        p->buf[(p->head + p->count) % PIPE_SIZE] = src[n++];
        p->count++;
    }
    waitq_wake_all(&p->readers);
    poll_notify();
    irq_restore(flags);
    return n ? (int64_t)n : ZK_EINTR;
}

static void maybe_free(struct pipe *p)
{
    if (!p->rd_open && !p->wr_open)
        kfree(p);
}

static void rd_release(struct vnode *v)
{
    struct pipe *p = v->priv;
    uint64_t flags = irq_save();
    p->rd_open = false;
    waitq_wake_all(&p->writers);
    maybe_free(p);
    irq_restore(flags);
}

static void wr_release(struct vnode *v)
{
    struct pipe *p = v->priv;
    uint64_t flags = irq_save();
    p->wr_open = false;
    waitq_wake_all(&p->readers);
    poll_notify();
    maybe_free(p);
    irq_restore(flags);
}

static int pipe_poll_rd(struct vnode *v)
{
    struct pipe *p = v->priv;
    int r = 0;
    if (p->count)
        r |= POLLIN;
    if (!p->wr_open)
        r |= POLLIN | POLLHUP;
    return r;
}

static int pipe_poll_wr(struct vnode *v)
{
    struct pipe *p = v->priv;
    if (!p->rd_open)
        return POLLHUP;
    return p->count < PIPE_SIZE ? POLLOUT : 0;
}

static const struct vnode_ops rd_ops = { .read = pipe_read, .release = rd_release, .poll = pipe_poll_rd };
static const struct vnode_ops wr_ops = { .write = pipe_write, .release = wr_release, .poll = pipe_poll_wr };

int pipe_create(struct file **rd, struct file **wr)
{
    struct pipe *p = kzalloc(sizeof(*p));
    if (!p)
        return ZK_ENOMEM;
    p->rd_open = p->wr_open = true;
    p->rd = (struct vnode){ .type = VNODE_PIPE, .refs = 1, .ino = vfs_next_ino(),
                            .ops = &rd_ops, .priv = p };
    p->wr = (struct vnode){ .type = VNODE_PIPE, .refs = 1, .ino = vfs_next_ino(),
                            .ops = &wr_ops, .priv = p };

    // the files take their own refs, then ours go away
    *rd = file_new(&p->rd, O_RDONLY);
    *wr = file_new(&p->wr, O_WRONLY);
    if (!*rd || !*wr) {
        if (*rd)
            file_put(*rd);
        if (*wr)
            file_put(*wr);
        vnode_put(&p->rd);
        vnode_put(&p->wr);
        return ZK_ENOMEM;
    }
    vnode_put(&p->rd);
    vnode_put(&p->wr);
    return 0;
}
