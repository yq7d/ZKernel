#pragma once
#include <stdbool.h>

// both sync the filesystems first
__attribute__((noreturn)) void machine_reboot(void);
__attribute__((noreturn)) void machine_poweroff(void);

// live usb mode: the boot stick going away wipes ram and powers off, like
// tails. armed at boot when started from a disk rather than a cd
void wipe_arm(bool on);
bool wipe_armed(void);
__attribute__((noreturn)) void emergency_wipe(const char *why);
