#pragma once
#include <stdbool.h>
#include <stdint.h>

bool mouse_init(void);
// movement from another mouse (usb), dy positive going down
void mouse_report(int dx, int dy, uint8_t buttons);
// /dev/mouse
extern struct vnode mouse_vnode;
