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
#include "drivers/serial.h"
#include "kprintf.h"
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

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

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

    pr_info("running self-tests...\n");
    bool ok = test_string() & test_printf();
    if (!ok)
        pr_warn("some self-tests failed\n");

    char fault[16];
    if (cmdline_get("fault", fault, sizeof fault))
        trigger_fault(fault);

    pr_ok("boot complete, halting.\n");
    hcf();
}
