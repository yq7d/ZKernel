#pragma once
#include <stdbool.h>

// keys without an ascii value
enum {
    KEY_UP = 0x100,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_HOME,
    KEY_END,
    KEY_PGUP,
    KEY_PGDN,
    KEY_INSERT,
    KEY_DELETE,
    KEY_F1,     // KEY_F1 + n for F1-F12
};

bool keyboard_init(void);

// blocks until a key is pressed. returns ascii or a KEY_* code,
// -1 if the calling process got killed
int keyboard_getc(void);
// a key as if it was typed: to the console tty, or the kernel shell queue.
// irq context or interrupts off
void keyboard_inject(int c);
// -1 if nothing is waiting
int keyboard_trygetc(void);

// /dev/kbd, raw key events. opening it takes the keyboard from the console
extern struct vnode kbd_vnode;
