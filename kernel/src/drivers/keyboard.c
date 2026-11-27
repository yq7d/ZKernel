// ps/2 keyboard, scancode set 1 (the controller translates from set 2), us layout
#include "drivers/keyboard.h"
#include "drivers/ps2.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpu.h"
#include "lib/spinlock.h"
#include "drivers/tty.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include <stdint.h>

#define SC_EXTENDED 0xE0
#define SC_RELEASE  0x80
#define SC_LCTRL    0x1D
#define SC_LSHIFT   0x2A
#define SC_RSHIFT   0x36
#define SC_LALT     0x38
#define SC_CAPS     0x3A
#define SC_F1       0x3B
#define SC_F10      0x44
#define SC_F11      0x57
#define SC_F12      0x58

static const char keymap[128] = {
    0, 27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
    '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0,
    '*', 0, ' ', 0,
    [0x47] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

static const char keymap_shift[128] = {
    0, 27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
    '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0, '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0,
    '*', 0, ' ', 0,
    [0x47] = '7', '8', '9', '-', '4', '5', '6', '+', '1', '2', '3', '0', '.',
};

#define BUF_SIZE    256
#define EVENTS_SIZE 128

// kernel shell queue
static volatile int buf[BUF_SIZE];
static volatile uint8_t head, tail;    // uint8_t wraps at BUF_SIZE
static struct waitq readers = WAITQ_INIT;

static bool lshift, rshift, ctrl, alt, super, caps, extended;

// raw events for /dev/kbd. while it's open the keys go nowhere else
static struct zk_key_event events[EVENTS_SIZE];
static size_t ev_head, ev_count;
static struct waitq ev_readers = WAITQ_INIT;
static int grabbed;

static void queue_shell(int c)
{
    uint8_t next = head + 1;
    if (next == tail)
        return;    // full, drop it
    buf[head] = c;
    head = next;
    waitq_wake_all(&readers);
}

// irq context. a typed character or KEY_* code
void keyboard_inject(int c)
{
    if (!tty_console_routed()) {
        queue_shell(c);
        return;
    }
    if (c < 0x100) {
        tty_input(&console_tty, (char)c);
        return;
    }
    // special keys only mean something to raw mode programs
    if (console_tty.mode & ZK_TTY_CANON)
        return;
    static const char *const seqs[] = {
        [KEY_UP - KEY_UP] = "\e[A", [KEY_DOWN - KEY_UP] = "\e[B",
        [KEY_RIGHT - KEY_UP] = "\e[C", [KEY_LEFT - KEY_UP] = "\e[D",
        [KEY_HOME - KEY_UP] = "\e[H", [KEY_END - KEY_UP] = "\e[F",
        [KEY_PGUP - KEY_UP] = "\e[5~", [KEY_PGDN - KEY_UP] = "\e[6~",
        [KEY_INSERT - KEY_UP] = "\e[2~", [KEY_DELETE - KEY_UP] = "\e[3~",
    };
    if (c - KEY_UP < (int)(sizeof seqs / sizeof seqs[0]) && seqs[c - KEY_UP])
        for (const char *p = seqs[c - KEY_UP]; *p; p++)
            tty_input(&console_tty, *p);
}

static uint32_t mods(void)
{
    return (lshift || rshift ? KEYMOD_SHIFT : 0) | (ctrl ? KEYMOD_CTRL : 0) |
           (alt ? KEYMOD_ALT : 0) | (super ? KEYMOD_SUPER : 0) | (caps ? KEYMOD_CAPS : 0);
}

static void push_event(uint16_t code, bool pressed, int key)
{
    if (ev_count == EVENTS_SIZE)
        return;
    struct zk_key_event *e = &events[(ev_head + ev_count++) % EVENTS_SIZE];
    e->code = code;
    e->pressed = pressed;
    e->mods = mods();
    e->key = key > 0 ? (uint32_t)key : 0;
    e->ch = key > 0 && key < 0x100 ? (uint8_t)key : 0;
    waitq_wake_all(&ev_readers);
    poll_notify();
}

// what a key means with the current modifiers, 0 for nothing
static int translate(uint8_t code, bool ext)
{
    if (ext) {
        switch (code) {
        case 0x1C: return '\n';    // keypad enter
        case 0x35: return '/';
        case 0x47: return KEY_HOME;
        case 0x48: return KEY_UP;
        case 0x49: return KEY_PGUP;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x4F: return KEY_END;
        case 0x50: return KEY_DOWN;
        case 0x51: return KEY_PGDN;
        case 0x52: return KEY_INSERT;
        case 0x53: return KEY_DELETE;
        }
        return 0;
    }
    if (code >= SC_F1 && code <= SC_F10)
        return KEY_F1 + (code - SC_F1);
    if (code == SC_F11 || code == SC_F12)
        return KEY_F1 + 10 + (code - SC_F11);
    if (code >= 128)
        return 0;

    bool shift = lshift || rshift;
    char c = keymap[code];
    if (!c)
        return 0;
    bool letter = c >= 'a' && c <= 'z';
    if (letter ? (shift != caps) : shift)
        c = keymap_shift[code];
    if (ctrl && letter)
        c &= 0x1F;
    return (unsigned char)c;
}

static void handle_scancode(uint8_t sc)
{
    if (sc == SC_EXTENDED) {
        extended = true;
        return;
    }
    bool released = sc & SC_RELEASE;
    uint8_t code = sc & ~SC_RELEASE;
    bool ext = extended;
    extended = false;

    if (ext) {
        switch (code) {
        case SC_LCTRL: ctrl = !released; break;
        case SC_LALT:  alt = !released; break;
        case 0x5B:
        case 0x5C:     super = !released; break;
        }
    } else {
        switch (code) {
        case SC_LSHIFT: lshift = !released; break;
        case SC_RSHIFT: rshift = !released; break;
        case SC_LCTRL:  ctrl = !released; break;
        case SC_LALT:   alt = !released; break;
        case SC_CAPS:
            if (!released)
                caps = !caps;
            break;
        }
    }

    int key = released ? 0 : translate(code, ext);
    if (grabbed) {
        push_event((uint16_t)(code | (ext ? 0x80 : 0)), !released, key);
        return;
    }
    if (key)
        keyboard_inject(key);
}

static void keyboard_irq(struct interrupt_frame *frame)
{
    (void)frame;
    handle_scancode(inb(PS2_DATA));
}

bool keyboard_init(void)
{
    if (!ps2_init())
        return false;
    irq_set_handler(1, keyboard_irq);
    return true;
}

int keyboard_trygetc(void)
{
    uint64_t flags = irq_save();
    int c = -1;
    if (tail != head) {
        c = buf[tail];
        tail++;
    }
    irq_restore(flags);
    return c;
}

int keyboard_getc(void)
{
    uint64_t flags = irq_save();
    while (tail == head) {
        if (proc_killed()) {
            irq_restore(flags);
            return -1;
        }
        waitq_sleep(&readers);
    }
    int c = buf[tail];
    tail++;
    irq_restore(flags);
    return c;
}

// /dev/kbd

static int kbd_open(struct vnode *v, struct file *f)
{
    (void)v; (void)f;
    uint64_t flags = irq_save();
    grabbed++;
    ev_count = 0;
    irq_restore(flags);
    return 0;
}

static void kbd_close(struct vnode *v, struct file *f)
{
    (void)v; (void)f;
    uint64_t flags = irq_save();
    grabbed--;
    irq_restore(flags);
}

static int64_t kbd_read(struct vnode *v, void *out, size_t len, uint64_t off)
{
    (void)v; (void)off;
    size_t want = len / sizeof(struct zk_key_event);
    if (want == 0)
        return ZK_EINVAL;
    struct zk_key_event *dst = out;
    uint64_t flags = irq_save();
    while (ev_count == 0) {
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&ev_readers);
    }
    size_t n = 0;
    while (n < want && ev_count) {
        dst[n++] = events[ev_head];
        ev_head = (ev_head + 1) % EVENTS_SIZE;
        ev_count--;
    }
    irq_restore(flags);
    return (int64_t)(n * sizeof(struct zk_key_event));
}

static int kbd_poll(struct vnode *v)
{
    (void)v;
    return ev_count ? POLLIN : 0;
}

static const struct vnode_ops kbd_ops = {
    .read = kbd_read,
    .open = kbd_open,
    .close = kbd_close,
    .poll = kbd_poll,
};

struct vnode kbd_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &kbd_ops };
