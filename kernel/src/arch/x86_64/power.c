#include "arch/x86_64/power.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/io.h"
#include "drivers/ps2.h"
#include "fs/vfs.h"
#include "kprintf.h"

void machine_reboot(void)
{
    vfs_sync();
    kprintf("rebooting...\n");
    acpi_reset();
    ps2_reboot();

    // controller didn't do it, triple fault instead
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) empty = { 0, 0 };
    __asm__ volatile ("lidt %0; int3" : : "m"(empty));
    hcf();
}

void machine_poweroff(void)
{
    vfs_sync();
    kprintf("powering off...\n");
    cli();
    acpi_poweroff();
    // qemu's fixed ports, for when acpi couldn't be read: q35, i440fx, bochs
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);
    outw(0x4004, 0x3400);
    kprintf("it is now safe to turn off your computer\n");
    hcf();
}
