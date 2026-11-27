#pragma once
// linux style input devices, /dev/input/event*
#include <stdbool.h>
#include <stdint.h>
#include "fs/vfs.h"
#include "sched/sched.h"

#define EV_SYN 0
#define EV_KEY 1
#define EV_REL 2
#define EV_REP 0x14

#define SYN_REPORT 0
#define REL_X      0
#define REL_Y      1
#define REL_WHEEL  8
#define BTN_LEFT   0x110
#define BTN_RIGHT  0x111
#define BTN_MIDDLE 0x112

struct input_event {
    int64_t sec, usec;
    uint16_t type, code;
    int32_t value;
};

#define EVDEV_QUEUE 256

struct evdev {
    const char *name;
    uint16_t product;
    uint8_t evbits[4];
    uint8_t keybits[96];    // KEY_MAX 0x2ff
    uint8_t relbits[2];
    uint8_t keystate[96];
    int clock;              // CLOCK_REALTIME unless EVIOCSCLOCKID
    int opens;
    struct input_event q[EVDEV_QUEUE];
    uint32_t head, count;
    struct waitq readers;
    struct vnode vnode;
};

extern struct evdev evdev_kbd, evdev_mouse;

void evdev_init(void);
// irq context. call evdev_sync after a group of events
void evdev_push(struct evdev *d, uint16_t type, uint16_t code, int32_t value);
void evdev_sync(struct evdev *d);
static inline bool evdev_open(struct evdev *d) { return d->opens > 0; }
