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

// blocks until a key is pressed. returns ascii or a KEY_* code
int keyboard_getc(void);
// -1 if nothing is waiting
int keyboard_trygetc(void);
