// ps/2 mouse, 3 byte packets, read as events from /dev/mouse
#include "drivers/mouse.h"
#include "drivers/ps2.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "drivers/evdev.h"

#define EVENTS 128

static uint8_t packet[3];
static int got;
static struct zk_mouse_event events[EVENTS];
static size_t head, count;
static struct waitq readers = WAITQ_INIT;

static void push_evdev(int dx, int dy, uint8_t buttons)
{
    static uint8_t last;
    struct evdev *d = &evdev_mouse;
    if (dx)
        evdev_push(d, EV_REL, REL_X, dx);
    if (dy)
        evdev_push(d, EV_REL, REL_Y, dy);
    static const uint16_t btn[3] = { BTN_LEFT, BTN_RIGHT, BTN_MIDDLE };
    for (int i = 0; i < 3; i++)
        if ((buttons ^ last) & (1 << i))
            evdev_push(d, EV_KEY, btn[i], (buttons >> i) & 1);
    last = buttons;
    evdev_sync(d);
}

static void push(int dx, int dy, uint8_t buttons)
{
    push_evdev(dx, dy, buttons);
    // merge plain movement into the last event when nobody's keeping up
    if (count) {
        struct zk_mouse_event *last = &events[(head + count - 1) % EVENTS];
        if (last->buttons == buttons && count == EVENTS) {
            last->dx = (int16_t)(last->dx + dx);
            last->dy = (int16_t)(last->dy + dy);
            return;
        }
    }
    if (count == EVENTS)
        return;
    events[(head + count++) % EVENTS] = (struct zk_mouse_event){
        .dx = (int16_t)dx, .dy = (int16_t)dy, .buttons = buttons,
    };
    waitq_wake_all(&readers);
    poll_notify();
}

static void mouse_irq(struct interrupt_frame *f)
{
    (void)f;
    uint8_t b = inb(PS2_DATA);
    // the first byte always has bit 3 set, use it to stay in sync
    if (got == 0 && !(b & 0x08))
        return;
    packet[got++] = b;
    if (got < 3)
        return;
    got = 0;
    if (packet[0] & 0xC0)
        return;    // overflow, the deltas are junk
    int dx = packet[1] - ((packet[0] << 4) & 0x100);
    int dy = packet[2] - ((packet[0] << 3) & 0x100);
    push(dx, -dy, packet[0] & 7);
}

bool mouse_init(void)
{
    if (!ps2_mouse_init())
        return false;
    irq_set_handler(12, mouse_irq);
    return true;
}

static int64_t m_read(struct vnode *v, void *out, size_t len, uint64_t off)
{
    (void)v; (void)off;
    size_t want = len / sizeof(struct zk_mouse_event);
    if (want == 0)
        return ZK_EINVAL;
    struct zk_mouse_event *dst = out;
    uint64_t flags = irq_save();
    while (count == 0) {
        if (proc_killed()) {
            irq_restore(flags);
            return ZK_EINTR;
        }
        waitq_sleep(&readers);
    }
    size_t n = 0;
    while (n < want && count) {
        dst[n++] = events[head];
        head = (head + 1) % EVENTS;
        count--;
    }
    irq_restore(flags);
    return (int64_t)(n * sizeof(struct zk_mouse_event));
}

static int m_open(struct vnode *v, struct file *f)
{
    (void)v; (void)f;
    uint64_t flags = irq_save();
    count = 0;    // stale movement from before
    irq_restore(flags);
    return 0;
}

static int m_poll(struct vnode *v)
{
    (void)v;
    return count ? POLLIN : 0;
}

static const struct vnode_ops ops = { .read = m_read, .open = m_open, .poll = m_poll };
struct vnode mouse_vnode = { .type = VNODE_DEV, .refs = 1, .ops = &ops };
