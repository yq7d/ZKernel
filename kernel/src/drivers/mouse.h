#pragma once
#include <stdbool.h>

bool mouse_init(void);
// /dev/mouse
extern struct vnode mouse_vnode;
