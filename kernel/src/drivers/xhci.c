// xhci host controllers and the usb keyboards and mice on their ports.
// devices are driven in their boot protocol, which every keyboard and mouse
// speaks: keys become set 1 scancodes and motion goes to the mouse driver,
// so everything above sees them like ps/2 ones. no interrupts, a thread
// polls the event rings every millisecond and the ports twice a second.
// hubs are followed, their ports polled with control requests
#include "drivers/xhci.h"
#include "arch/x86_64/cpu.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"

#define MAX_HC    4
#define MAX_DEVS  16
#define RING_TRBS 64      // one page, the last one links back
#define EV_TRBS   256

// trb types
#define TRB_NORMAL     1
#define TRB_SETUP      2
#define TRB_DATA       3
#define TRB_STATUS     4
#define TRB_LINK       6
#define TRB_ENABLE     9
#define TRB_ADDRESS    11
#define TRB_CONFIGURE  12
#define TRB_EVALUATE   13
#define TRB_EV_XFER    32
#define TRB_EV_CMD     33
#define TRB_EV_PORT    34

#define TRB_IOC  (1U << 5)
#define TRB_IDT  (1U << 6)
#define TRB_TC   (1U << 1)    // link: toggle cycle

// portsc
#define PORT_CCS  (1U << 0)
#define PORT_PED  (1U << 1)
#define PORT_PR   (1U << 4)
#define PORT_PP   (1U << 9)
#define PORT_CHANGES 0x00FE0000U
#define PORT_PRC  (1U << 21)

struct trb {
    uint64_t param;
    uint32_t status;
    uint32_t control;
};

struct ring {
    struct trb *trbs;
    uint64_t phys;
    uint32_t idx;
    uint32_t cycle;
};

struct hc;

struct dev {
    struct hc *hc;
    int slot, port, speed;  // port: on the root hub
    uint32_t route;         // hub ports on the way, 4 bits a level
    int depth;
    int tt_slot, tt_port;   // the high speed hub a low/full speed device hangs off
    int hub_ports;          // nonzero for hubs
    bool hub_ss;            // a usb 3 hub
    uint32_t hub_attached;  // bit per hub port with a device on it
    struct ring ep0;
    struct ring in;         // the hid interrupt endpoint
    int in_dci;
    int kind;               // 1 keyboard, 2 mouse
    uint8_t *buf;           // report buffer, one page
    uint64_t buf_phys;
    uint16_t in_mps;
    uint8_t last[8];        // previous keyboard report
    uint64_t repeat_at;     // typematic repeat for the last key pressed
    uint8_t repeat_key;
    bool waiting;           // a transfer is queued on the interrupt ring
};

struct hc {
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    int ports, slots, csz;   // csz: context size in bytes, 32 or 64
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *ev;
    uint64_t ev_phys;
    uint32_t ev_idx, ev_cycle;
    struct dev *devs[256];   // by slot
    bool attached[256];      // by port
    // the last command and control transfer completions, for the waits
    bool cmd_done, xfer_done;
    uint32_t cmd_code, cmd_slot, xfer_code;
};

static struct hc *hcs[MAX_HC];
static int nhc;

static uint32_t rd32(volatile uint8_t *b, uint32_t off) { return *(volatile uint32_t *)(b + off); }
static void wr32(volatile uint8_t *b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b + off) = v; }
static void wr64(volatile uint8_t *b, uint32_t off, uint64_t v)
{
    *(volatile uint32_t *)(b + off) = (uint32_t)v;
    *(volatile uint32_t *)(b + off + 4) = (uint32_t)(v >> 32);
}

static void *dma_page(uint64_t *phys)
{
    *phys = pmm_alloc_zeroed();
    return *phys ? phys_to_virt(*phys) : NULL;
}

static bool ring_init(struct ring *r)
{
    r->trbs = dma_page(&r->phys);
    if (!r->trbs)
        return false;
    r->idx = 0;
    r->cycle = 1;
    struct trb *link = &r->trbs[RING_TRBS - 1];
    link->param = r->phys;
    link->control = (TRB_LINK << 10) | TRB_TC;
    return true;
}

// queues one trb, returns its physical address
static uint64_t ring_push(struct ring *r, uint64_t param, uint32_t status, uint32_t control)
{
    struct trb *t = &r->trbs[r->idx];
    uint64_t at = r->phys + r->idx * sizeof(struct trb);
    t->param = param;
    t->status = status;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    t->control = control | r->cycle;
    if (++r->idx == RING_TRBS - 1) {
        struct trb *link = &r->trbs[RING_TRBS - 1];
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        link->control = (TRB_LINK << 10) | TRB_TC | r->cycle;
        r->idx = 0;
        r->cycle ^= 1;
    }
    return at;
}

static void ring_doorbell(struct hc *h, int slot, uint32_t target)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    h->db[slot] = target;
}

static uint32_t *ctx(struct hc *h, void *base, int index)
{
    return (uint32_t *)((uint8_t *)base + (size_t)index * (size_t)h->csz);
}

// keyboards

// hid usage to set 1 scancode, 0x100 marks the e0 prefix
static const uint16_t usage_sc[0x66] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
    0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    [0x1E] = 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    [0x28] = 0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28,
    0x29, 0x33, 0x34, 0x35, 0x3A,
    [0x3A] = 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,
    [0x46] = 0x137, 0x46, 0,
    [0x49] = 0x152, 0x147, 0x149, 0x153, 0x14F, 0x151, 0x14D, 0x14B, 0x150, 0x148,
    [0x53] = 0x45, 0x135, 0x37, 0x4A, 0x4E, 0x11C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D,
    0x47, 0x48, 0x49, 0x52, 0x53, 0x56, 0x15D,
};

// modifier bits of the report: lctrl lshift lalt lgui rctrl rshift ralt rgui
static const uint16_t mod_sc[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

static void send_sc(uint16_t sc, bool down)
{
    if (!sc)
        return;
    if (sc & 0x100)
        keyboard_scancode(0xE0);
    keyboard_scancode((uint8_t)((sc & 0x7F) | (down ? 0 : 0x80)));
}

static bool has(const uint8_t *keys, uint8_t k)
{
    for (int i = 2; i < 8; i++)
        if (keys[i] == k)
            return true;
    return false;
}

static void keyboard_report(struct dev *d, const uint8_t *r)
{
    if (r[2] == 1)
        return;    // rollover error, nothing in it is real
    uint8_t mods = r[0] ^ d->last[0];
    for (int b = 0; b < 8; b++)
        if (mods & (1 << b))
            send_sc(mod_sc[b], r[0] & (1 << b));
    for (int i = 2; i < 8; i++) {
        uint8_t k = d->last[i];
        if (k > 3 && k < 0x66 && !has(r, k)) {
            send_sc(usage_sc[k], false);
            if (k == d->repeat_key)
                d->repeat_key = 0;
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k > 3 && k < 0x66 && !has(d->last, k)) {
            send_sc(usage_sc[k], true);
            d->repeat_key = k;
            d->repeat_at = timer_uptime_ms() + 500;
        }
    }
    memcpy(d->last, r, 8);
}

// usb keyboards leave repeating to the host
static void keyboard_repeat(struct dev *d)
{
    if (!d->repeat_key || timer_uptime_ms() < d->repeat_at)
        return;
    send_sc(usage_sc[d->repeat_key], true);
    d->repeat_at = timer_uptime_ms() + 33;
}

// events

static void queue_in(struct dev *d)
{
    ring_push(&d->in, d->buf_phys, d->in_mps, (TRB_NORMAL << 10) | TRB_IOC);
    d->waiting = true;
    ring_doorbell(d->hc, d->slot, (uint32_t)d->in_dci);
}

static void handle_event(struct hc *h, struct trb *e)
{
    uint32_t type = (e->control >> 10) & 63;
    uint32_t code = e->status >> 24;
    uint32_t slot = e->control >> 24;
    if (type == TRB_EV_CMD) {
        h->cmd_done = true;
        h->cmd_code = code;
        h->cmd_slot = slot;
    } else if (type == TRB_EV_XFER) {
        uint32_t dci = (e->control >> 16) & 31;
        struct dev *d = h->devs[slot];
        if (dci == 1 || !d) {
            h->xfer_done = true;
            h->xfer_code = code;
            return;
        }
        if ((int)dci != d->in_dci)
            return;
        d->waiting = false;
        if (code == 1 || code == 13) {    // success, short packet
            size_t got = d->in_mps - (e->status & 0xFFFFFF);
            if (d->kind == 1 && got >= 8)
                keyboard_report(d, d->buf);
            else if (d->kind == 2 && got >= 3)
                mouse_report((int8_t)d->buf[1], (int8_t)d->buf[2], d->buf[0] & 7);
        }
        if (code == 1 || code == 13)
            queue_in(d);
    }
    // port changes are picked up by the scan in the thread
}

static void poll_events(struct hc *h)
{
    bool any = false;
    for (;;) {
        struct trb *e = &h->ev[h->ev_idx];
        if ((e->control & 1) != h->ev_cycle)
            break;
        handle_event(h, e);
        any = true;
        if (++h->ev_idx == EV_TRBS) {
            h->ev_idx = 0;
            h->ev_cycle ^= 1;
        }
    }
    if (any) {
        uint64_t at = h->ev_phys + h->ev_idx * sizeof(struct trb);
        wr64(h->rt, 0x38, at | (1 << 3));    // erdp, clearing the busy bit
    }
}

static bool wait_cmd(struct hc *h, uint32_t *slot)
{
    for (uint64_t end = timer_uptime_ms() + 500; timer_uptime_ms() < end;) {
        poll_events(h);
        if (h->cmd_done) {
            h->cmd_done = false;
            if (slot)
                *slot = h->cmd_slot;
            return h->cmd_code == 1;
        }
        thread_sleep_ms(1);
    }
    return false;
}

static bool command(struct hc *h, uint64_t param, uint32_t control, uint32_t *slot)
{
    h->cmd_done = false;
    ring_push(&h->cmd, param, 0, control);
    ring_doorbell(h, 0, 0);
    return wait_cmd(h, slot);
}

// a control transfer on endpoint 0, len bytes in or out of buf
static bool control(struct dev *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index,
                    uint16_t len, uint64_t buf)
{
    struct hc *h = d->hc;
    bool in = type & 0x80;
    uint64_t setup = type | (uint64_t)req << 8 | (uint64_t)value << 16 |
                     (uint64_t)index << 32 | (uint64_t)len << 48;
    uint32_t trt = len ? (in ? 3 : 2) : 0;
    ring_push(&d->ep0, setup, 8, (TRB_SETUP << 10) | TRB_IDT | (trt << 16));
    if (len)
        ring_push(&d->ep0, buf, len, (TRB_DATA << 10) | (in ? 1U << 16 : 0));
    ring_push(&d->ep0, 0, 0, (TRB_STATUS << 10) | TRB_IOC | (len && in ? 0 : 1U << 16));
    h->xfer_done = false;
    ring_doorbell(h, d->slot, 1);
    for (uint64_t end = timer_uptime_ms() + 1000; timer_uptime_ms() < end;) {
        poll_events(h);
        if (h->xfer_done)
            return h->xfer_code == 1 || h->xfer_code == 13;
        thread_sleep_ms(1);
    }
    return false;
}

// devices

static int ep0_mps(int speed)
{
    switch (speed) {
    case 2: return 8;      // low
    case 3: return 64;     // high
    case 4: case 5: return 512;
    default: return 8;     // full, fixed up from the descriptor
    }
}

// xhci intervals are 2^n * 125us
static int interval(int speed, uint8_t b)
{
    if (speed >= 3)
        return b ? (b > 16 ? 15 : b - 1) : 0;
    int n = 3;
    while (n < 10 && (1 << (n - 3)) < b)
        n++;
    return n;
}

static void setup_hid(struct dev *d, const uint8_t *cfg, size_t len)
{
    struct hc *h = d->hc;
    int iface = -1, kind = 0, ep = 0;
    uint16_t mps = 8;
    uint8_t binterval = 10;
    for (size_t off = 0; off + 2 <= len && cfg[off] >= 2; off += cfg[off]) {
        const uint8_t *x = cfg + off;
        if (x[1] == 4 && off + 9 <= len) {    // interface
            if (kind && ep)
                break;
            kind = 0;
            if (x[5] == 3 && x[6] == 1 && (x[7] == 1 || x[7] == 2)) {
                iface = x[2];
                kind = x[7];
            }
        } else if (x[1] == 5 && kind && !ep && off + 7 <= len) {    // endpoint
            if ((x[2] & 0x80) && (x[3] & 3) == 3) {
                ep = x[2] & 15;
                mps = (uint16_t)((x[4] | x[5] << 8) & 0x7FF);
                binterval = x[6];
            }
        }
    }
    if (!kind || !ep)
        return;

    uint8_t cfgval = cfg[5];
    if (!control(d, 0x00, 9, cfgval, 0, 0, 0))    // set configuration
        return;
    control(d, 0x21, 0x0B, 0, (uint16_t)iface, 0, 0);    // set protocol: boot
    control(d, 0x21, 0x0A, 0, (uint16_t)iface, 0, 0);    // set idle

    if (!ring_init(&d->in))
        return;
    d->in_dci = ep * 2 + 1;
    d->in_mps = mps < 8 ? 8 : mps;
    d->kind = kind;

    uint64_t inphys;
    void *input = dma_page(&inphys);
    if (!input)
        return;
    uint32_t *icc = ctx(h, input, 0);
    icc[1] = 1U | (1U << d->in_dci);    // add slot and the endpoint
    uint32_t *slot = ctx(h, input, 1);
    uint64_t outphys = h->dcbaa[d->slot];
    memcpy(slot, ctx(h, phys_to_virt(outphys), 0), (size_t)h->csz);
    slot[0] = (slot[0] & ~(31U << 27)) | ((uint32_t)d->in_dci << 27);
    uint32_t *e = ctx(h, input, 1 + d->in_dci);
    e[0] = (uint32_t)interval(d->speed, binterval) << 16;
    e[1] = (3U << 1) | (7U << 3) | ((uint32_t)mps << 16);    // cerr 3, interrupt in
    e[2] = (uint32_t)d->in.phys | 1;
    e[3] = (uint32_t)(d->in.phys >> 32);
    e[4] = mps | ((uint32_t)mps << 16);
    bool ok = command(h, inphys, (TRB_CONFIGURE << 10) | ((uint32_t)d->slot << 24), NULL);
    pmm_free(inphys);
    if (!ok)
        return;
    pr_ok("usb: %s on port %d\n", kind == 1 ? "keyboard" : "mouse", d->port);
    queue_in(d);
}

static void scan_hub(struct dev *hub);
static void attach(struct hc *h, int port, int speed, struct dev *parent, int hub_port);

// hub port requests
#define HUB_SET 0x23
#define HUB_CLEAR 0x23
#define F_PORT_RESET 4
#define F_PORT_POWER 8
#define F_C_CONNECTION 16
#define F_C_RESET 20

static void setup_hub(struct dev *d, const uint8_t *cfg)
{
    struct hc *h = d->hc;
    if (d->depth >= 5 || !control(d, 0x00, 9, cfg[5], 0, 0, 0))
        return;
    d->hub_ss = d->speed >= 4;
    if (d->hub_ss)
        control(d, 0x20, 12, (uint16_t)d->depth, 0, 0, 0);    // set hub depth
    uint8_t *buf = d->buf;
    uint16_t type = d->hub_ss ? 0x2A00 : 0x2900;
    if (!control(d, 0xA0, 6, type, 0, 9, d->buf_phys))
        return;
    int ports = buf[2];
    int settle = buf[5] * 2;
    if (ports < 1)
        return;
    if (ports > 15)
        ports = 15;

    // tell the controller it's a hub
    uint64_t inphys;
    void *input = dma_page(&inphys);
    if (!input)
        return;
    uint32_t *icc = ctx(h, input, 0);
    icc[1] = 1;
    uint32_t *slot = ctx(h, input, 1);
    memcpy(slot, ctx(h, phys_to_virt(h->dcbaa[d->slot]), 0), (size_t)h->csz);
    slot[0] |= 1U << 26;
    slot[1] = (slot[1] & 0x00FFFFFF) | ((uint32_t)ports << 24);
    if (d->speed == 3)
        slot[2] &= ~(3U << 16);    // tt think time 8 fs bit times
    bool ok = command(h, inphys, (TRB_EVALUATE << 10) | ((uint32_t)d->slot << 24), NULL) ||
              command(h, inphys, (TRB_CONFIGURE << 10) | ((uint32_t)d->slot << 24), NULL);
    pmm_free(inphys);
    (void)ok;    // some controllers want one, some the other

    d->hub_ports = ports;
    for (int p = 1; p <= ports; p++)
        control(d, HUB_SET, 3, F_PORT_POWER, (uint16_t)p, 0, 0);
    thread_sleep_ms((uint64_t)(settle > 100 ? settle : 100));
    pr_ok("usb: hub with %d ports on port %d\n", ports, d->port);
    scan_hub(d);
}

// what changed on each hub port: connections get reset and attached
static void scan_hub(struct dev *hub)
{
    struct hc *h = hub->hc;
    for (int p = 1; p <= hub->hub_ports; p++) {
        if (!control(hub, 0xA3, 0, 0, (uint16_t)p, 4, hub->buf_phys))
            continue;
        uint16_t status = (uint16_t)(hub->buf[0] | hub->buf[1] << 8);
        uint16_t change = (uint16_t)(hub->buf[2] | hub->buf[3] << 8);
        if (change & 1)
            control(hub, HUB_CLEAR, 1, F_C_CONNECTION, (uint16_t)p, 0, 0);
        bool on = status & 1;
        uint32_t bit = 1U << p;
        if (!on) {
            hub->hub_attached &= ~bit;
            continue;
        }
        if (hub->hub_attached & bit)
            continue;
        hub->hub_attached |= bit;
        thread_sleep_ms(100);    // let the plug settle
        control(hub, HUB_SET, 3, F_PORT_RESET, (uint16_t)p, 0, 0);
        bool reset = false;
        for (int i = 0; i < 50 && !reset; i++) {
            thread_sleep_ms(10);
            if (control(hub, 0xA3, 0, 0, (uint16_t)p, 4, hub->buf_phys))
                reset = (hub->buf[2] | hub->buf[3] << 8) & (1 << 4);
        }
        control(hub, HUB_CLEAR, 1, F_C_RESET, (uint16_t)p, 0, 0);
        thread_sleep_ms(20);
        if (!control(hub, 0xA3, 0, 0, (uint16_t)p, 4, hub->buf_phys))
            continue;
        status = (uint16_t)(hub->buf[0] | hub->buf[1] << 8);
        if (!(status & 2))
            continue;    // didn't enable
        int speed = hub->hub_ss ? 4 : (status & (1 << 9)) ? 2 : (status & (1 << 10)) ? 3 : 1;
        attach(h, hub->port, speed, hub, p);
    }
}

static void attach(struct hc *h, int port, int speed, struct dev *parent, int hub_port)
{
    uint32_t slot;
    if (!command(h, 0, TRB_ENABLE << 10, &slot) || !slot || slot > (uint32_t)h->slots)
        return;
    struct dev *d = kzalloc(sizeof *d);
    uint64_t outphys, inphys;
    void *out = dma_page(&outphys), *input = dma_page(&inphys);
    if (!d || !out || !input || !ring_init(&d->ep0) || !(d->buf = dma_page(&d->buf_phys))) {
        kfree(d);
        return;
    }
    d->hc = h;
    d->slot = (int)slot;
    d->port = port;
    d->speed = speed;
    if (parent) {
        d->depth = parent->depth + 1;
        d->route = parent->route | (uint32_t)(hub_port > 15 ? 15 : hub_port) << (4 * parent->depth);
        // low and full speed devices on a high speed hub go through its tt
        if (speed <= 2 && parent->speed == 3) {
            d->tt_slot = parent->slot;
            d->tt_port = hub_port;
        } else {
            d->tt_slot = parent->tt_slot;
            d->tt_port = parent->tt_port;
        }
    }
    h->dcbaa[slot] = outphys;

    // input context: slot and endpoint 0
    uint32_t *icc = ctx(h, input, 0);
    icc[1] = 3;
    uint32_t *sc = ctx(h, input, 1);
    sc[0] = d->route | ((uint32_t)speed << 20) | (1U << 27);
    sc[1] = (uint32_t)port << 16;
    sc[2] = (uint32_t)d->tt_slot | ((uint32_t)d->tt_port << 8);
    uint32_t *e0 = ctx(h, input, 2);
    e0[1] = (3U << 1) | (4U << 3) | ((uint32_t)ep0_mps(speed) << 16);
    e0[2] = (uint32_t)d->ep0.phys | 1;
    e0[3] = (uint32_t)(d->ep0.phys >> 32);
    e0[4] = 8;
    h->devs[slot] = d;
    if (!command(h, inphys, (TRB_ADDRESS << 10) | (slot << 24), NULL)) {
        pmm_free(inphys);
        return;
    }

    // device descriptor, first 8 bytes for the real endpoint 0 size
    uint8_t *buf = d->buf;
    if (!control(d, 0x80, 6, 0x100, 0, 8, d->buf_phys)) {
        pmm_free(inphys);
        return;
    }
    uint8_t dev_class = buf[4];
    if (speed == 1 && buf[7] && buf[7] != 8) {
        icc[1] = 2;
        e0[1] = (e0[1] & 0xFFFF) | ((uint32_t)buf[7] << 16);
        command(h, inphys, (TRB_EVALUATE << 10) | (slot << 24), NULL);
    }
    pmm_free(inphys);

    // configuration descriptor, all of it
    if (!control(d, 0x80, 6, 0x200, 0, 9, d->buf_phys))
        return;
    uint16_t total = (uint16_t)(buf[2] | buf[3] << 8);
    if (total > PAGE_SIZE)
        total = PAGE_SIZE;
    if (!control(d, 0x80, 6, 0x200, 0, total, d->buf_phys))
        return;
    uint8_t *cfg = kmalloc(total);
    if (!cfg)
        return;
    memcpy(cfg, buf, total);
    if (dev_class == 9)
        setup_hub(d, cfg);
    else
        setup_hid(d, cfg, total);
    kfree(cfg);
}

static volatile uint32_t *portsc(struct hc *h, int port)
{
    return (volatile uint32_t *)(h->op + 0x400 + 0x10 * (port - 1));
}

static void port_write(struct hc *h, int port, uint32_t set)
{
    uint32_t v = *portsc(h, port) & ~(PORT_PED | PORT_CHANGES);
    *portsc(h, port) = v | set;
}

// new connections get reset (usb 2) or are already enabled (usb 3)
static void scan_ports(struct hc *h)
{
    for (int p = 1; p <= h->ports; p++) {
        uint32_t v = *portsc(h, p);
        if (v & PORT_CHANGES)
            port_write(h, p, v & PORT_CHANGES);    // acknowledge
        bool on = v & PORT_CCS;
        if (!on) {
            h->attached[p] = false;
            continue;
        }
        if (h->attached[p])
            continue;
        h->attached[p] = true;
        if (!(v & PORT_PED)) {
            port_write(h, p, PORT_PR);
            uint64_t end = timer_uptime_ms() + 200;
            while (!(*portsc(h, p) & PORT_PRC) && timer_uptime_ms() < end)
                thread_sleep_ms(1);
            port_write(h, p, PORT_PRC);
            thread_sleep_ms(20);
            v = *portsc(h, p);
            if (!(v & PORT_PED))
                continue;
        }
        attach(h, p, (int)((v >> 10) & 15), NULL, 0);
    }
    // hubs answer requests, they don't raise port events here
    for (int s = 1; s <= h->slots; s++)
        if (h->devs[s] && h->devs[s]->hub_ports)
            scan_hub(h->devs[s]);
}

static void usb_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < nhc; i++)
        scan_ports(hcs[i]);
    uint64_t next_scan = timer_uptime_ms() + 500;
    for (;;) {
        for (int i = 0; i < nhc; i++) {
            struct hc *h = hcs[i];
            poll_events(h);
            for (int s = 1; s <= h->slots; s++)
                if (h->devs[s] && h->devs[s]->kind == 1)
                    keyboard_repeat(h->devs[s]);
        }
        if (timer_uptime_ms() >= next_scan) {
            for (int i = 0; i < nhc; i++)
                scan_ports(hcs[i]);
            next_scan = timer_uptime_ms() + 500;
        }
        thread_sleep_ms(1);
    }
}

// the firmware lets go of the controller (and stops its ps/2 emulation)
static void take_over(struct hc *h, uint32_t hcc)
{
    uint32_t off = (hcc >> 16) * 4;
    while (off) {
        uint32_t c = rd32(h->cap, off);
        if ((c & 0xFF) == 1) {
            wr32(h->cap, off, c | (1U << 24));
            for (uint64_t end = timer_uptime_ms() + 1000;
                 (rd32(h->cap, off) & (1U << 16)) && timer_uptime_ms() < end;)
                ;
            wr32(h->cap, off, rd32(h->cap, off) & ~(1U << 16));
            wr32(h->cap, off + 4, 0xE0000000);    // smis off, their status cleared
        }
        uint32_t next = (c >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

static bool start(const struct pci_dev *pd)
{
    bool io;
    uint64_t bar = pci_bar(pd, 0, &io);
    if (io || !bar)
        return false;
    pci_enable(pd);
    struct hc *h = kzalloc(sizeof *h);
    if (!h)
        return false;
    h->cap = vmm_map_mmio(bar, 0x10000);
    if (!h->cap) {
        kfree(h);
        return false;
    }
    uint32_t hcs1 = rd32(h->cap, 4), hcs2 = rd32(h->cap, 8), hcc = rd32(h->cap, 0x10);
    h->op = h->cap + (rd32(h->cap, 0) & 0xFF);
    h->rt = h->cap + (rd32(h->cap, 0x18) & ~0x1FU);
    h->db = (volatile uint32_t *)(h->cap + (rd32(h->cap, 0x14) & ~3U));
    h->slots = (int)(hcs1 & 0xFF);
    if (h->slots > MAX_DEVS)
        h->slots = MAX_DEVS;
    h->ports = (int)(hcs1 >> 24);
    h->csz = (hcc & 4) ? 64 : 32;

    take_over(h, hcc);

    // stop, reset
    wr32(h->op, 0, rd32(h->op, 0) & ~1U);
    for (uint64_t end = timer_uptime_ms() + 100; !(rd32(h->op, 4) & 1) && timer_uptime_ms() < end;)
        ;
    wr32(h->op, 0, 2);
    for (uint64_t end = timer_uptime_ms() + 1000;
         (rd32(h->op, 0) & 2 || rd32(h->op, 4) & (1U << 11)) && timer_uptime_ms() < end;)
        ;
    if (rd32(h->op, 4) & (1U << 11)) {
        pr_warn("usb: controller %02x:%02x.%x won't come out of reset\n", pd->bus, pd->dev, pd->fn);
        return false;
    }

    uint64_t dphys;
    h->dcbaa = dma_page(&dphys);
    if (!h->dcbaa || !ring_init(&h->cmd))
        return false;
    // scratchpad pages the controller asked for
    uint32_t scratch = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (scratch) {
        uint64_t aphys;
        uint64_t *arr = dma_page(&aphys);
        if (!arr || scratch > 512)
            return false;
        for (uint32_t i = 0; i < scratch; i++)
            arr[i] = pmm_alloc_zeroed();
        h->dcbaa[0] = aphys;
    }
    wr32(h->op, 0x38, (uint32_t)h->slots);
    wr64(h->op, 0x30, dphys);
    wr64(h->op, 0x18, h->cmd.phys | 1);

    // one event ring segment
    uint64_t segphys;
    uint64_t *seg = dma_page(&segphys);
    h->ev = NULL;
    uint64_t evphys = pmm_alloc_pages(EV_TRBS * sizeof(struct trb) / PAGE_SIZE);
    if (!seg || !evphys)
        return false;
    memset(phys_to_virt(evphys), 0, EV_TRBS * sizeof(struct trb));
    h->ev = phys_to_virt(evphys);
    h->ev_phys = evphys;
    h->ev_cycle = 1;
    seg[0] = evphys;
    seg[1] = EV_TRBS;
    wr32(h->rt, 0x28, 1);                  // erstsz
    wr64(h->rt, 0x38, evphys);             // erdp
    wr64(h->rt, 0x30, segphys);            // erstba
    wr32(h->rt, 0x20, 1);                  // iman: pending bit cleared, no interrupts, we poll

    wr32(h->op, 0, 1);                     // run
    for (uint64_t end = timer_uptime_ms() + 100; (rd32(h->op, 4) & 1) && timer_uptime_ms() < end;)
        ;
    // ports with power control start unpowered
    for (int p = 1; p <= h->ports; p++)
        if (!(*portsc(h, p) & PORT_PP))
            port_write(h, p, PORT_PP);
    hcs[nhc++] = h;
    pr_ok("usb: xhci %02x:%02x.%x, %d ports\n", pd->bus, pd->dev, pd->fn, h->ports);
    return true;
}

bool xhci_init(void)
{
    for (int i = 0; i < pci_count() && nhc < MAX_HC; i++) {
        const struct pci_dev *d = pci_get(i);
        if (d->class == 0x0C && d->subclass == 0x03 && d->prog_if == 0x30)
            start(d);
    }
    if (!nhc)
        return false;
    thread_create("usb", usb_thread, NULL, false);
    return true;
}
