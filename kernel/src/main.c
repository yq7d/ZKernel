#include "arch/x86_64/power.h"
#include "drivers/xhci.h"
#include "fs/sysfs.h"
#include <stdint.h>
#include <stddef.h>
#include <limine.h>
#include <zk/version.h>

#include "arch/x86_64/acpi.h"
#include "arch/x86_64/fpu.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/pic.h"
#include "cmdline.h"
#include "console/console.h"
#include "drivers/ahci.h"
#include "drivers/blk.h"
#include "drivers/nvme.h"
#include "drivers/fb.h"
#include "drivers/keyboard.h"
#include "drivers/mouse.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "drivers/virtio_blk.h"
#include "drivers/rtc.h"
#include "drivers/serial.h"
#include "fs/vfs.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "kprintf.h"
#include "drivers/tty.h"
#include "lib/string.h"
#include "net/net.h"
#include "proc/linux.h"
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

__attribute__((used, section(".limine_requests")))
static volatile struct limine_module_request module_request = {
    .id = LIMINE_MODULE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_executable_file_request kfile_request = {
    .id = LIMINE_EXECUTABLE_FILE_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests")))
static volatile struct limine_rsdp_request rsdp_request = {
    .id = LIMINE_RSDP_REQUEST_ID,
    .revision = 0,
};

__attribute__((used, section(".limine_requests_start")))
static volatile uint64_t requests_start[] = LIMINE_REQUESTS_START_MARKER;

__attribute__((used, section(".limine_requests_end")))
static volatile uint64_t requests_end[] = LIMINE_REQUESTS_END_MARKER;

#define KERNEL_STACK_PAGES 16

extern void switch_stack(uint64_t top, void (*fn)(void)) __attribute__((noreturn));

static uint64_t boot_stack;

// module memory isn't reclaimed, but the limine structs describing it are
static uint64_t rsdp_phys;
static const void *initrd_data;
static size_t initrd_size;
static const void *apps_data;
static size_t apps_size;

static void find_initrd(void)
{
    struct limine_module_response *r = module_request.response;
    if (!r)
        return;
    for (uint64_t i = 0; i < r->module_count; i++) {
        const char *path = r->modules[i]->path;
        const char *slash = strrchr(path, '/');
        if (strcmp(slash ? slash + 1 : path, "initrd.tar") == 0) {
            initrd_data = r->modules[i]->address;
            initrd_size = r->modules[i]->size;
        } else if (strcmp(slash ? slash + 1 : path, "apps.img") == 0) {
            apps_data = r->modules[i]->address;
            apps_size = r->modules[i]->size;
        }
    }
}

// apps.img from the iso: a read only ext2 image in memory, the bundled
// linux programs. linux programs look in /compat/apps after /compat/linux
static int ram_read(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    if ((lba + count) * SECTOR_SIZE > d->sectors * SECTOR_SIZE)
        return ZK_EIO;
    memcpy(buf, (const uint8_t *)d->priv + lba * SECTOR_SIZE, (size_t)count * SECTOR_SIZE);
    return 0;
}

static int ram_write(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    (void)d; (void)lba; (void)count; (void)buf;
    return ZK_EROFS;
}

static void mount_apps(void)
{
    if (!apps_data)
        return;
    static struct blkdev ram = { .name = "ram0", .read = ram_read, .write = ram_write,
                                 .model = "bundled apps" };
    ram.sectors = apps_size / SECTOR_SIZE;
    ram.priv = (void *)apps_data;
    blk_register(&ram);
    vfs_mkdir("/compat");
    vfs_mkdir("/compat/apps");
    if (vfs_mount("/dev/ram0", "/compat/apps", "ext2", "ro") == 0)
        pr_ok("apps: %zu MiB of bundled programs on /compat/apps\n", apps_size >> 20);
    else
        pr_warn("apps: can't mount apps.img\n");
}

// runs /bin/sh (or init=) on the console, falling back to the kernel shell
static void run_init(void)
{
    char init[ZK_PATH_MAX] = "/bin/init";
    cmdline_get("init", init, sizeof init);
    if (strcmp(init, "kshell") == 0)
        return;
    tty_console_route(true);

    for (int failures = 0; failures < 3; ) {
        struct spawn_opts o;
        proc_console_opts(&o);
        char *argv[] = { init, NULL };
        int err;
        uint64_t start = timer_uptime_ms();
        struct thread *t = proc_spawn(init, argv, proc_default_env, &o, &err);
        proc_put_console(&o);
        if (!t) {
            pr_err("init: can't run %s: %s\n", init, proc_strerror(err));
            tty_console_route(false);
            return;
        }
        int code = thread_join(t);
        console_tty.fg = 0;
        pr_warn("init: %s exited with %d, restarting\n", init, code);
        // something is badly wrong if it keeps dying right away
        failures = timer_uptime_ms() - start < 1000 ? failures + 1 : 0;
    }
    pr_err("init: giving up\n");
    tty_console_route(false);
}

static void banner(void)
{
    kprintf("\e[1;34m"
            "  _____ _  __                    _\n"
            " |__  /| |/ /___ _ __ _ __   ___| |\n"
            "   / / | ' // _ \\ '__| '_ \\ / _ \\ |\n"
            "  / /_ | . \\  __/ |  | | | |  __/ |\n"
            " /____||_|\\_\\___|_|  |_| |_|\\___|_|\n"
            "\e[0m\n"
            "  \e[1mZKernel\e[0m " ZK_VERSION " x86_64 - built " __DATE__ " " __TIME__ "\n\n");
}

// runs on the new kernel stack
static void kmain_late(void)
{
    // the old stack and everything limine handed over are now unused
    size_t reclaimed = pmm_reclaim_bootloader(memmap_request.response);
    pr_ok("pmm: reclaimed %zu KiB of bootloader memory\n", reclaimed * PAGE_SIZE >> 10);

    sched_init(boot_stack, KERNEL_STACK_PAGES);
    pr_ok("sched: running as thread \"%s\"\n", thread_current()->name);

    vfs_init();
    if (initrd_data) {
        int n = initrd_unpack(initrd_data, initrd_size);
        pr_ok("initrd: %d files, %zu KiB\n", n, initrd_size >> 10);
    } else {
        pr_warn("initrd: no initrd.tar module\n");
    }
    syscall_init();
    linux_init();
    pr_ok("syscalls: int 0x80, plus the syscall instruction for linux programs\n");

    acpi_init(rsdp_phys);
    pci_init();
    pr_ok("pci: %d devices\n", pci_count());

    net_init();

    timer_init(TIMER_HZ);
    pr_ok("timer: pit at %d Hz\n", TIMER_HZ);
    rtc_init();
    pr_ok("rtc: unix time %ld\n", rtc_now());
    serial_enable_input();
    if (keyboard_init())
        pr_ok("keyboard: ps/2 ready\n");
    else
        pr_warn("keyboard: no ps/2 controller found\n");
    if (mouse_init())
        pr_ok("mouse: ps/2 ready\n");
    else
        pr_warn("mouse: none found\n");

    sysfs_init();

    __asm__ volatile ("sti");
    pr_ok("interrupts enabled\n");

    // disk drivers wait on the timer, so they come after it
    virtio_blk_init();
    ahci_init();
    nvme_init();
    blk_start_flusher();
    mount_apps();

    pr_info("running self-tests...\n");
    bool ok = test_string() & test_printf() & test_mm() & test_timer() & test_sched() & test_vfs() & test_proc();
    if (!ok)
        pr_warn("some self-tests failed\n");

    char fault[16];
    if (cmdline_get("fault", fault, sizeof fault))
        trigger_fault(fault);

    // after the self tests, which count free pages: usb devices being set up
    // meanwhile would throw them off. "nousb" leaves usb to the firmware,
    // its legacy emulation keeps working
    char nousb[4];
    if (cmdline_get("nousb", nousb, sizeof nousb))
        pr_info("usb: left to the firmware (nousb)\n");
    else
        xhci_init();

    pr_ok("boot complete\n");
    run_init();
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
    tty_console_init();

    cmdline_init(cmdline_request.response ? cmdline_request.response->cmdline : "");
    pr_info("command line: \"%s\"\n", cmdline_raw());

    fpu_init();
    pr_ok("fpu: sse enabled\n");
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
    find_initrd();
    // booted off a usb stick or disk rather than a cd: unplugging it wipes
    // ram and powers off, unless "nowipe". read now, limine's memory is
    // reclaimed later
    struct limine_executable_file_response *kf = kfile_request.response;
    char nowipe[4];
    wipe_arm(kf && kf->executable_file &&
             kf->executable_file->media_type == LIMINE_MEDIA_TYPE_GENERIC &&
             !cmdline_get("nowipe", nowipe, sizeof nowipe));
    // the response sits in memory that's about to be reclaimed
    if (rsdp_request.response) {
        // older protocol revisions hand out an hhdm address, newer a physical one
        rsdp_phys = (uint64_t)rsdp_request.response->address;
        if (rsdp_phys >= hhdm_offset)
            rsdp_phys -= hhdm_offset;
    }

    boot_stack = vmm_alloc_stack(KERNEL_STACK_PAGES);
    pr_ok("stack: %u KiB at %p with guard page\n", KERNEL_STACK_PAGES * 4, (void *)boot_stack);
    switch_stack(boot_stack, kmain_late);
}
