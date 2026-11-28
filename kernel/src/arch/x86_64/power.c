#include "arch/x86_64/power.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/io.h"
#include "drivers/ps2.h"
#include "fs/vfs.h"
#include "kprintf.h"
#include "drivers/fb.h"
#include "lib/string.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

static bool armed;

void wipe_arm(bool on)
{
    armed = on;
}

bool wipe_armed(void)
{
    return armed;
}

// everything below lives in the kernel image, not in the ram being wiped
#define MAX_TABLES 32768
static uint64_t tables[MAX_TABLES];
static size_t ntables;
static uint8_t wipe_stack[16384] __attribute__((aligned(16)));

__attribute__((noreturn)) static void wipe_and_off(void)
{
    memset(fb.addr, 0, (size_t)fb.pitch * fb.height);
    pmm_wipe(tables, ntables);
    acpi_poweroff();
    outw(0x604, 0x2000);    // qemu
    outb(0x64, 0xFE);       // or reset through the keyboard controller
    for (;;)
        __asm__ volatile ("cli; hlt");
}

void emergency_wipe(const char *why)
{
    kprintf("\n%s: wiping memory and powering off\n", why);
    cli();
    vmm_activate(NULL);
    ntables = vmm_kernel_tables(tables, MAX_TABLES);
    // sorted, for the lookups while wiping
    for (size_t i = 1; i < ntables; i++) {
        uint64_t v = tables[i];
        size_t j = i;
        for (; j > 0 && tables[j - 1] > v; j--)
            tables[j] = tables[j - 1];
        tables[j] = v;
    }
    __asm__ volatile ("mov %0, %%rsp; call *%1"
                      : : "r"(wipe_stack + sizeof wipe_stack), "r"(wipe_and_off) : "memory");
    hcf();
}

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
