#include <stdint.h>
#include <stddef.h>
#include <limine.h>

#include "arch/x86_64/gdt.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "cmdline.h"
#include "console/console.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "drivers/serial.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "kprintf.h"
#include "proc/proc.h"
#include "sched/sched.h"
#include "shell.h"
#include "tests/tests.h"

__attribute__((used, section(".limine_requests")))
static volatile uint64_t limine_base_revision[] = LIMINE_BASE_REVISION(4);

__attribute__((used, section(".limine_requests")))
static volatile struct limine_framebuffer_request fb_request = {
    .id = LIMINE_FRAMEBUFFER_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_cmdline_request cmdline_request = {
    .id = LIMINE_EXECUTABLE_CMDLINE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_memmap_request memmap_request = {
    .id = LIMINE_MEMMAP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_hhdm_request hhdm_request = {
    .id = LIMINE_HHDM_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_address_request kaddr_request = {
    .id = LIMINE_EXECUTABLE_ADDRESS_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

#define KERNEL_STACK_PAGES 16

extern void switch_stack(uint64_t top, void (*fn)(void)) __attribute__((noreturn));

static uint64_t boot_stack;

static void banner(void)
{
    kprintf("\e[1;34m"
            "  _____ _  __                    _\n"
            " |__  /| |/ /___ _ __ _ __   ___| |\n"
            "   / / | ' // _ \\ '__| '_ \\ / _ \\ |\n"
            "  / /_ | . \\  __/ |  | | | |  __/ |\n"
            " /____||_|\\_\\___|_|  |_| |_|\\___|_|\n"
            "\e[0m\n"
            "  \e[1mZKernel\e[0m x86_64 - built " __DATE__ " " __TIME__ "\n\n");
}

// runs on the new kernel stack
static void kmain_late(void)
{
    // the old stack and everything limine handed over are now unused
    size_t reclaimed = pmm_reclaim_bootloader(memmap_request.response);
    pr_ok("pmm: reclaimed %zu KiB of bootloader memory\n", reclaimed * PAGE_SIZE >> 10);

    sched_init(boot_stack, KERNEL_STACK_PAGES);
    pr_ok("sched: running as thread \"%s\"\n", thread_current()->name);
    syscall_init();
    pr_ok("syscalls: int 0x80\n");

    timer_init(TIMER_HZ);
    pr_ok("timer: pit at %d Hz\n", TIMER_HZ);
    if (keyboard_init())
        pr_ok("keyboard: ps/2 ready\n");
    else
        pr_warn("keyboard: no ps/2 controller found\n");

    __asm__ volatile ("sti");
    pr_ok("interrupts enabled\n");

    pr_info("running self-tests...\n");
    bool ok = test_string() & test_printf() & test_mm() & test_timer() & test_sched() & test_proc();
    if (!ok)
        pr_warn("some self-tests failed\n");

    char fault[16];
    if (cmdline_get("fault", fault, sizeof fault))
        trigger_fault(fault);

    pr_ok("boot complete\n");
    shell_run();
}

void kmain(void)
{
    serial_init();

    if (!LIMINE_BASE_REVISION_SUPPORTED(limine_base_revision))
        panic("bootloader does not support Limine base revision 4");

    struct limine_framebuffer_response *fbr = fb_request.response;
    if (fbr == NULL || fbr->framebuffer_count < 1)
        panic("no framebuffer available");

    struct limine_framebuffer *lfb = fbr->framebuffers[0];
    if (!fb_init(lfb))
        panic("unsupported framebuffer format (%u bpp)", lfb->bpp);
    if (!console_init())
        panic("failed to load console font");

    banner();
    pr_ok("serial: COM1 at 115200 baud\n");
    pr_ok("framebuffer: %ux%u, pitch %u, at %p\n",
          fb.width, fb.height, fb.pitch, (void *)fb.addr);
    pr_ok("console: ready\n");

    cmdline_init(cmdline_request.response ? cmdline_request.response->cmdline : "");
    pr_info("command line: \"%s\"\n", cmdline_raw());

    gdt_init();
    pr_ok("gdt: kernel/user segments + tss loaded\n");
    pic_init(IRQ_BASE);
    pr_ok("pic: remapped to vectors %d-%d, all lines masked\n", IRQ_BASE, IRQ_BASE + 15);
    idt_init();
    pr_ok("idt: 256 vectors installed\n");

    if (!memmap_request.response || !hhdm_request.response || !kaddr_request.response)
        panic("missing memory map / hhdm / kernel address from bootloader");
    hhdm_offset = hhdm_request.response->offset;

    pmm_init(memmap_request.response);
    pr_ok("pmm: %zu MiB usable, %zu MiB free\n",
          pmm_total_pages() * PAGE_SIZE >> 20, pmm_free_count() * PAGE_SIZE >> 20);

    vmm_init(memmap_request.response, kaddr_request.response->physical_base,
             kaddr_request.response->virtual_base);
    pr_ok("vmm: own page tables loaded\n");

    heap_init();
    pr_ok("heap: ready at %p\n", (void *)HEAP_BASE);

    boot_stack = vmm_alloc_stack(KERNEL_STACK_PAGES);
    pr_ok("stack: %u KiB at %p with guard page\n", KERNEL_STACK_PAGES * 4, (void *)boot_stack);
    switch_stack(boot_stack, kmain_late);
}
