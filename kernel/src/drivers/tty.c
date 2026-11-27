// console tty: line editing and echo, input from the keyboard and com1
#include "drivers/tty.h"
#include "drivers/keyboard.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "lib/string.h"
#include "proc/proc.h"
#include "sched/sync.h"

#define LINE_MAX 256
#define WRITE_CHUNK 512

static struct mutex read_lock = MUTEX_INIT;
static char line[LINE_MAX];
static size_t line_len;
static char cooked[LINE_MAX];
static size_t cooked_len, cooked_pos;
static volatile uint64_t fg_pgid;

void tty_set_fg(uint64_t pgid)
{
    fg_pgid = pgid;
}

uint64_t tty_get_fg(void)
{
    return fg_pgid;
}

// irq context
bool tty_ctrl_c(void)
{
    if (!fg_pgid || !proc_kill_group(fg_pgid))
        return false;
    console_out("^C\n", 3);
    return true;
}

static void echo(const char *s)
{
    console_out(s, strlen(s));
}

// returns 1 for a finished line, 0 for eof, or an error
static int edit_line(void)
{
    for (;;) {
        if (proc_killed())
            return ZK_EINTR;
        int c = keyboard_getc();
        if (c < 0)
            return ZK_EINTR;

        if (c == '\n') {
            echo("\n");
            line[line_len++] = '\n';
            return 1;
        }
        if (c == '\b' || c == 0x7f) {
            if (line_len) {
                line_len--;
                echo("\b \b");
            }
            continue;
        }
        if (c == 3) {    // ctrl+c
            echo("^C\n");
            line_len = 0;
            if (proc_killed())
                return ZK_EINTR;
            continue;
        }
        if (c == 4) {    // ctrl+d
            if (line_len == 0)
                return 0;
            return 1;
        }
        if (c == 21) {   // ctrl+u
            while (line_len) {
                line_len--;
                echo("\b \b");
            }
            continue;
        }
        if ((c == '\t' || (c >= 0x20 && c < 0x7f)) && line_len + 1 < LINE_MAX) {
            line[line_len++] = (char)c;
            char ch = (char)c;
            console_out(&ch, 1);
        }
    }
}

static int64_t tty_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    (void)v; (void)off;
    if (len == 0)
        return 0;

    mutex_lock(&read_lock);
    if (cooked_pos == cooked_len) {
        int r = edit_line();
        if (r < 0)
            line_len = 0;    // a killed reader's half line goes with it
        if (r <= 0) {
            mutex_unlock(&read_lock);
            return r;
        }
        memcpy(cooked, line, line_len);
        cooked_len = line_len;
        cooked_pos = 0;
        line_len = 0;
    }
    size_t n = cooked_len - cooked_pos;
    if (n > len)
        n = len;
    memcpy(buf, cooked + cooked_pos, n);
    cooked_pos += n;
    mutex_unlock(&read_lock);
    return (int64_t)n;
}

static int64_t tty_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)v; (void)off;
    const char *s = buf;
    for (size_t done = 0; done < len; ) {
        size_t n = len - done < WRITE_CHUNK ? len - done : WRITE_CHUNK;
        console_out(s + done, n);
        done += n;
    }
    return (int64_t)len;
}

static const struct vnode_ops tty_ops = { .read = tty_read, .write = tty_write };

struct vnode tty_vnode = {
    .type = VNODE_DEV,
    .refs = 1,    // never released
    .ops = &tty_ops,
};
