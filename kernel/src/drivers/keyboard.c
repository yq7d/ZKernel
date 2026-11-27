// ps/2 keyboard, scancode set 1 (the controller translates from set 2), us layout
#include "drivers/keyboard.h"
#include "drivers/ps2.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/cpu.h"
#include "lib/spinlock.h"
#include "drivers/tty.h"
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

#define BUF_SIZE 256

static volatile int buf[BUF_SIZE];
static volatile uint8_t head, tail;    // uint8_t wraps at BUF_SIZE

static bool lshift, rshift, ctrl, alt, caps, extended;
static struct waitq readers = WAITQ_INIT;

// irq context
void keyboard_inject(int c)
{
    // killed readers have to notice, the key itself is used up
    if (c == 3 && tty_ctrl_c()) {
        waitq_wake_all(&readers);
        return;
    }
    uint8_t next = head + 1;
    if (next == tail)
        return;    // full, drop it
    buf[head] = c;
    head = next;
    waitq_wake_all(&readers);
}

static void handle_extended(uint8_t code, bool released)
{
    if (code == SC_LCTRL) {
        ctrl = !released;
        return;
    }
    if (code == SC_LALT) {
        alt = !released;
        return;
    }
    if (released)
        return;

    switch (code) {
    case 0x1C: keyboard_inject('\n'); break;    // keypad enter
    case 0x35: keyboard_inject('/'); break;     // keypad /
    case 0x47: keyboard_inject(KEY_HOME); break;
    case 0x48: keyboard_inject(KEY_UP); break;
    case 0x49: keyboard_inject(KEY_PGUP); break;
    case 0x4B: keyboard_inject(KEY_LEFT); break;
    case 0x4D: keyboard_inject(KEY_RIGHT); break;
    case 0x4F: keyboard_inject(KEY_END); break;
    case 0x50: keyboard_inject(KEY_DOWN); break;
    case 0x51: keyboard_inject(KEY_PGDN); break;
    case 0x52: keyboard_inject(KEY_INSERT); break;
    case 0x53: keyboard_inject(KEY_DELETE); break;
    }
}

static void handle_scancode(uint8_t sc)
{
    if (sc == SC_EXTENDED) {
        extended = true;
        return;
    }

    bool released = sc & SC_RELEASE;
    uint8_t code = sc & ~SC_RELEASE;

    if (extended) {
        extended = false;
        handle_extended(code, released);
        return;
    }

    switch (code) {
    case SC_LSHIFT: lshift = !released; return;
    case SC_RSHIFT: rshift = !released; return;
    case SC_LCTRL:  ctrl = !released; return;
    case SC_LALT:   alt = !released; return;
    case SC_CAPS:
        if (!released)
            caps = !caps;
        return;
    }
    if (released)
        return;

    if (code >= SC_F1 && code <= SC_F10) {
        keyboard_inject(KEY_F1 + (code - SC_F1));
        return;
    }
    if (code == SC_F11 || code == SC_F12) {
        keyboard_inject(KEY_F1 + 10 + (code - SC_F11));
        return;
    }

    bool shift = lshift || rshift;
    char c = keymap[code];
    if (!c)
        return;

    bool letter = c >= 'a' && c <= 'z';
    if (letter ? (shift != caps) : shift)
        c = keymap_shift[code];
    if (ctrl && letter)
        c &= 0x1F;
    keyboard_inject(c);
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
    uint64_t flags = read_rflags();
    cli();
    int c = -1;
    if (tail != head) {
        c = buf[tail];
        tail++;
    }
    if (flags & RFLAGS_IF)
        sti();
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
