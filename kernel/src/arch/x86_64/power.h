#pragma once

// both sync the filesystems first
__attribute__((noreturn)) void machine_reboot(void);
__attribute__((noreturn)) void machine_poweroff(void);
