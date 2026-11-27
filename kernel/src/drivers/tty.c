#include "drivers/tty.h"
#include "arch/x86_64/cpu.h"
#include "console/console.h"
#include "drivers/fb.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "lib/string.h"
#include "proc/proc.h"

struct tty console_tty;
static bool routed;

void tty_init(struct tty *t, const struct tty_ops *ops, void *priv)
{
    memset(t, 0, sizeof(*t));
    t->ops = ops;
    t->priv = priv;
    t->mode = ZK_TTY_COOKED;
    t->ws = (struct zk_winsize){ 25, 80, 0, 0 };
}

static void echo(struct tty *t, const char *s, size_t n)
{
    if (t->mode & ZK_TTY_ECHO)
        t->ops->echo(t, s, n);
}

// interrupts off
static void push(struct tty *t, char c)
{
    if (t->in_count == TTY_IN_SIZE)
        return;    // full, drop it
    t->in[(t->in_head + t->in_count++) % TTY_IN_SIZE] = c;
}

static void ready(struct tty *t)
{
    waitq_wake_all(&t->readers);
    poll_notify();
}

void tty_input(struct tty *t, char c)
{
    uint64_t flags = irq_save();

    if ((t->mode & ZK_TTY_ISIG) && c == 3) {
        echo(t, "^C\n", 3);
        t->line_len = 0;
        t->in_count = 0;
        if (t->fg)
            proc_kill_group(t->fg);
        ready(t);
        irq_restore(flags);
        return;
    }

    if (!(t->mode & ZK_TTY_CANON)) {
        push(t, c);
        echo(t, &c, 1);
        ready(t);
        irq_restore(flags);
        return;
    }

    if (c == '\r')
        c = '\n';
    if (c == '\n') {
        echo(t, "\n", 1);
        for (size_t i = 0; i < t->line_len; i++)
            push(t, t->line[i]);
        push(t, '\n');
        t->line_len = 0;
        ready(t);
    } else if (c == '\b' || c == 0x7f) {
        if (t->line_len) {
            t->line_len--;
            echo(t, "\b \b", 3);
        }
    } else if (c == 21) {    // ctrl+u
        while (t->line_len) {
            t->line_len--;
            echo(t, "\b \b", 3);
        }
    } else if (c == 4) {     // ctrl+d
        if (t->line_len == 0) {
            t->eofs++;
        } else {
            for (size_t i = 0; i < t->line_len; i++)
                push(t, t->line[i]);
            t->line_len = 0;
        }
        ready(t);
    } else if ((c == '\t' || ((unsigned char)c >= 0x20 && c != 0x7f)) &&
               t->line_len + 1 < TTY_LINE_MAX) {
        t->line[t->line_len++] = c;
        echo(t, &c, 1);
    }
    irq_restore(flags);
}

int64_t tty_read(struct tty *t, void *buf, size_t len)
{
    if (len == 0)
        return 0;
    char *out = buf;
    uint64_t flags = irq_save();
    for (;;) {
        if (t->in_count)
            break;
        if (t->eofs) {
            t->eofs--;
            irq_restore(flags);
            return 0;
        }
        if (t->hangup) {
            irq_restore(flags);
            return 0;
        }
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&t->readers);
    }
    // a cooked read stops after one line
    size_t n = 0;
    while (n < len && t->in_count) {
        char c = t->in[t->in_head];
        t->in_head = (t->in_head + 1) % TTY_IN_SIZE;
        t->in_count--;
        out[n++] = c;
        if (c == '\n' && (t->mode & ZK_TTY_CANON))
            break;
    }
    irq_restore(flags);
    return (int64_t)n;
}

int64_t tty_write(struct tty *t, const void *buf, size_t len)
{
    if (t->hangup)
        return ZK_EIO;
    t->ops->write(t, buf, len);
    return (int64_t)len;
}

int tty_ioctl(struct tty *t, unsigned long req, uint64_t arg)
{
    switch (req) {
    case TIOCSFG:
        t->fg = arg;
        return 0;
    case TIOCGFG:
        return copy_out(arg, &t->fg, sizeof t->fg);
    case TIOCGWINSZ:
        return copy_out(arg, &t->ws, sizeof t->ws);
    case TIOCSWINSZ:
        return copy_in(&t->ws, arg, sizeof t->ws);
    case TIOCGMODE:
        return copy_out(arg, &t->mode, sizeof t->mode);
    case TIOCSMODE: {
        uint64_t flags = irq_save();
        // a half typed line goes to the reader when leaving canonical mode
        if ((t->mode & ZK_TTY_CANON) && !(arg & ZK_TTY_CANON)) {
            for (size_t i = 0; i < t->line_len; i++)
                push(t, t->line[i]);
            t->line_len = 0;
        }
        // back to lines: keys meant for the raw mode program are dropped
        if (!(t->mode & ZK_TTY_CANON) && (arg & ZK_TTY_CANON)) {
            t->in_count = 0;
            t->line_len = 0;
        }
        t->mode = (uint32_t)arg & ZK_TTY_COOKED;
        irq_restore(flags);
        return 0;
    }
    }
    return ZK_EINVAL;
}

int tty_poll(struct tty *t)
{
    int r = POLLOUT;
    if (t->in_count || t->eofs)
        r |= POLLIN;
    if (t->hangup)
        r |= POLLIN | POLLHUP;
    return r;
}

void tty_hangup(struct tty *t)
{
    uint64_t flags = irq_save();
    t->hangup = true;
    if (t->fg)
        proc_kill_group(t->fg);
    ready(t);
    irq_restore(flags);
}

// console

static void console_write_out(struct tty *t, const char *s, size_t n)
{
    (void)t;
    while (n) {
        size_t chunk = n < 512 ? n : 512;
        console_out(s, chunk);
        s += chunk;
        n -= chunk;
    }
}

static const struct tty_ops console_ops = { console_write_out, console_write_out };

void tty_console_init(void)
{
    tty_init(&console_tty, &console_ops, NULL);
    console_tty.ws.cols = (uint16_t)console_cols();
    console_tty.ws.rows = (uint16_t)console_rows();
    console_tty.ws.xpixels = (uint16_t)fb.width;
    console_tty.ws.ypixels = (uint16_t)fb.height;
}

void tty_console_route(bool on)
{
    routed = on;
}

bool tty_console_routed(void)
{
    return routed;
}

static int64_t con_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)v; (void)off;
    return tty_read(&console_tty, buf, len);
}

static int64_t con_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)v; (void)off;
    return tty_write(&console_tty, buf, len);
}

static int con_ioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)v; (void)f;
    return tty_ioctl(&console_tty, req, arg);
}

static int con_poll(struct vnode *v)
{
    (void)v;
    return tty_poll(&console_tty);
}

static const struct vnode_ops tty_vops = {
    .read = con_read,
    .write = con_write,
    .ioctl = con_ioctl,
    .poll = con_poll,
};

struct vnode tty_vnode = {
    .type = VNODE_DEV,
    .refs = 1,    // never released
    .ops = &tty_vops,
};

// /dev/tty: whichever terminal the process belongs to, a pty in a desktop
// window or the console otherwise
static int devtty_open(struct vnode *v, struct file *f)
{
    struct proc *p = proc_current();
    struct vnode *target = p && p->ctty ? p->ctty : &tty_vnode;
    vnode_get(target);
    f->v = target;
    vnode_put(v);
    return 0;
}

static const struct vnode_ops devtty_ops = { .open = devtty_open };

struct vnode devtty_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &devtty_ops };
