// /dev/input/event0 keyboard, event1 mouse. one queue per device, opening
// clears it
#include "drivers/evdev.h"
#include "arch/x86_64/cpu.h"
#include "drivers/pit.h"
#include "drivers/rtc.h"
#include "drivers/tty.h"
#include "kprintf.h"
#include "lib/string.h"
#include "proc/proc.h"

#define BIT(a, n) ((a)[(n) / 8] |= (uint8_t)(1 << ((n) % 8)))

struct evdev evdev_kbd = { .name = "AT Translated Set 2 keyboard", .product = 1 };
struct evdev evdev_mouse = { .name = "ImPS/2 Generic Wheel Mouse", .product = 2 };

static void stamp(struct evdev *d, struct input_event *e)
{
    uint64_t ms = timer_uptime_ms();
    e->usec = (int64_t)(ms % 1000) * 1000;
    e->sec = d->clock == 0 ? rtc_now() : (int64_t)(ms / 1000);
}

void evdev_push(struct evdev *d, uint16_t type, uint16_t code, int32_t value)
{
    if (type == EV_KEY && code < sizeof d->keystate * 8) {
        uint8_t bit = (uint8_t)(1 << (code % 8));
        bool down = d->keystate[code / 8] & bit;
        if (value && down)
            value = 2;    // typematic repeat
        else if (!value && !down)
            return;
        d->keystate[code / 8] = value ? d->keystate[code / 8] | bit : d->keystate[code / 8] & ~bit;
    }
    if (!d->opens || d->count == EVDEV_QUEUE)
        return;
    struct input_event *e = &d->q[(d->head + d->count++) % EVDEV_QUEUE];
    stamp(d, e);
    e->type = type;
    e->code = code;
    e->value = value;
}

void evdev_sync(struct evdev *d)
{
    if (!d->opens)
        return;
    evdev_push(d, EV_SYN, SYN_REPORT, 0);
    waitq_wake_all(&d->readers);
    poll_notify();
}

static struct evdev *of(struct vnode *v)
{
    return v->priv;
}

static int ev_open(struct vnode *v, struct file *f)
{
    (void)f;
    struct evdev *d = of(v);
    uint64_t irq = irq_save();
    d->opens++;
    d->count = 0;
    irq_restore(irq);
    return 0;
}

static void ev_close(struct vnode *v, struct file *f)
{
    (void)f;
    struct evdev *d = of(v);
    uint64_t irq = irq_save();
    d->opens--;
    irq_restore(irq);
}

static int64_t ev_read(struct vnode *v, void *out, size_t len, uint64_t off)
{
    (void)off;
    struct evdev *d = of(v);
    size_t want = len / sizeof(struct input_event);
    if (!want)
        return ZK_EINVAL;
    struct input_event *dst = out;
    uint64_t irq = irq_save();
    while (!d->count) {
        if (proc_killed()) {
            irq_restore(irq);
            return ZK_EINTR;
        }
        waitq_sleep(&d->readers);
    }
    size_t n = 0;
    while (n < want && d->count) {
        dst[n++] = d->q[d->head];
        d->head = (d->head + 1) % EVDEV_QUEUE;
        d->count--;
    }
    irq_restore(irq);
    return (int64_t)(n * sizeof *dst);
}

static int64_t ev_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    (void)v; (void)buf; (void)off;
    return (int64_t)len;    // leds and the like, ignored
}

static int ev_poll(struct vnode *v)
{
    return of(v)->count ? POLLIN : 0;
}

// copies a bitmap or string out, the size the caller asked for at most
static int put(uint64_t arg, size_t size, const void *src, size_t n)
{
    uint8_t buf[128];
    memset(buf, 0, sizeof buf);
    if (n > sizeof buf)
        n = sizeof buf;
    memcpy(buf, src, n);
    if (size > sizeof buf)
        size = sizeof buf;
    if (copy_out(arg, buf, size))
        return ZK_EFAULT;
    return (int)(n < size ? n : size);
}

static int ev_lioctl(struct vnode *v, struct file *f, unsigned long req, uint64_t arg)
{
    (void)f;
    struct evdev *d = of(v);
    if (((req >> 8) & 0xff) != 'E')
        return ZK_ENOTTY;
    unsigned nr = req & 0xff;
    size_t size = (req >> 16) & 0x3fff;
    static const uint8_t none[96];

    switch (nr) {
    case 0x01: {    // EVIOCGVERSION
        int32_t ver = 0x010001;
        return copy_out(arg, &ver, 4);
    }
    case 0x02: {    // EVIOCGID: bus i8042
        uint16_t id[4] = { 0x11, 1, d->product, 0xab41 };
        return copy_out(arg, id, sizeof id);
    }
    case 0x03: {    // EVIOCGREP
        uint32_t rep[2] = { 250, 33 };
        return copy_out(arg, rep, sizeof rep);
    }
    case 0x06:      // EVIOCGNAME
        return put(arg, size, d->name, strlen(d->name) + 1);
    case 0x07:      // EVIOCGPHYS
        return d == &evdev_kbd ? put(arg, size, "isa0060/serio0/input0", 22)
                               : put(arg, size, "isa0060/serio1/input0", 22);
    case 0x08:      // EVIOCGUNIQ
        return put(arg, size, "", 1);
    case 0x09:      // EVIOCGPROP
    case 0x19:      // EVIOCGLED
    case 0x1a:      // EVIOCGSND
    case 0x1b:      // EVIOCGSW
        return put(arg, size, none, size);
    case 0x18: {    // EVIOCGKEY
        uint64_t irq = irq_save();
        uint8_t ks[96];
        memcpy(ks, d->keystate, sizeof ks);
        irq_restore(irq);
        return put(arg, size, ks, sizeof ks);
    }
    case 0x90:      // EVIOCGRAB
    case 0xa0:      // EVIOCSCLOCKID
    case 0x91:      // EVIOCREVOKE
    case 0x92:      // EVIOCSMASK
        if (nr == 0xa0) {
            int32_t c;
            if (copy_in(&c, arg, 4))
                return ZK_EFAULT;
            d->clock = c;
        }
        return 0;
    }
    if (nr >= 0x20 && nr < 0x40) {    // EVIOCGBIT
        switch (nr - 0x20) {
        case 0:      return put(arg, size, d->evbits, sizeof d->evbits);
        case EV_KEY: return put(arg, size, d->keybits, sizeof d->keybits);
        case EV_REL: return put(arg, size, d->relbits, sizeof d->relbits);
        default:     return put(arg, size, none, 0);
        }
    }
    return ZK_EINVAL;    // EVIOCGABS and the rest: no absolute axes here
}

static const struct vnode_ops ops = {
    .read = ev_read, .write = ev_write, .open = ev_open, .close = ev_close,
    .poll = ev_poll, .lioctl = ev_lioctl,
};

static void setup(struct evdev *d)
{
    d->vnode.type = VNODE_DEV;
    d->vnode.refs = 1;
    d->vnode.ops = &ops;
    d->vnode.priv = d;
    BIT(d->evbits, EV_SYN);
    BIT(d->evbits, EV_KEY);
}

void evdev_init(void)
{
    setup(&evdev_kbd);
    BIT(evdev_kbd.evbits, EV_REP);
    for (int k = 1; k <= 88; k++)    // esc through f12, set 1 codes match
        BIT(evdev_kbd.keybits, k);
    for (int k = 96; k <= 127; k++)  // keypad enter, right ctrl/alt, arrows, meta...
        BIT(evdev_kbd.keybits, k);

    setup(&evdev_mouse);
    BIT(evdev_mouse.evbits, EV_REL);
    BIT(evdev_mouse.keybits, BTN_LEFT);
    BIT(evdev_mouse.keybits, BTN_RIGHT);
    BIT(evdev_mouse.keybits, BTN_MIDDLE);
    BIT(evdev_mouse.relbits, REL_X);
    BIT(evdev_mouse.relbits, REL_Y);
    BIT(evdev_mouse.relbits, REL_WHEEL);
}
