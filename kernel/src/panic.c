#include "panic.h"
#include "arch/x86_64/io.h"
#include "drivers/serial.h"
#include "kprintf.h"

#define KERNEL_BASE 0xffffffff80000000UL
#define HIGHER_HALF 0xffff800000000000UL
#define MAX_FRAMES  16

static volatile int panicking;

void panic_begin(void)
{
    __asm__ volatile ("cli");

    if (panicking++) {
        serial_puts("\n*** nested panic - halting ***\n");
        hcf();
    }

    kprintf("\n\e[1;41;37m KERNEL PANIC \e[0m ");
}

/* needs -fno-omit-frame-pointer. [rbp] = prev rbp, [rbp+8] = return addr */
void print_backtrace(uint64_t rbp)
{
    kprintf("  backtrace:\n");
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (rbp < HIGHER_HALF || (rbp & 7))
            break;
        uint64_t *frame = (uint64_t *)rbp;
        uint64_t ret = frame[1];
        if (ret < KERNEL_BASE)
            break;
        kprintf("    %p\n", (void *)ret);
        rbp = frame[0];
    }
    kprintf("  (resolve with: addr2line -fe build/kernel.elf <address>)\n");
}

void panic_at(const char *file, int line, const char *fmt, ...)
{
    panic_begin();

    kprintf("\e[1;31m");
    va_list ap;
    va_start(ap, fmt);
    kvprintf(fmt, ap);
    va_end(ap);
    kprintf("\e[0m\n  at %s:%d\n", file, line);

    print_backtrace((uint64_t)__builtin_frame_address(0));
    kprintf("  System halted.\n");
    hcf();
}
