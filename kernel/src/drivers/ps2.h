#pragma once
#include <stdbool.h>
#include <stdint.h>

#define PS2_DATA 0x60

// sets up the 8042 with port 1 enabled, irq 1 on, scancode translation on
bool ps2_init(void);
void ps2_reboot(void);
