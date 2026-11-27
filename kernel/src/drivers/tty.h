#pragma once
#include <stdbool.h>
#include <stdint.h>

struct vnode;
extern struct vnode tty_vnode;

// ctrl+c kills every process in this group. 0 for none
void tty_set_fg(uint64_t pgid);
uint64_t tty_get_fg(void);
// irq context. true if it killed something
bool tty_ctrl_c(void);
