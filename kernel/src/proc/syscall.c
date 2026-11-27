#include "proc/proc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/interrupts.h"
#include "drivers/keyboard.h"
#include "drivers/pit.h"
#include "kprintf.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "sched/sched.h"
#include <zk/syscall.h>

#define WRITE_CHUNK 1024
#define MAX_ALLOC_PAGES 4096

static struct address_space *cur_as(void)
{
    return thread_current()->as;
}

static int64_t sys_write(int fd, uint64_t buf, size_t len)
{
    if (fd != 1 && fd != 2)
        return ZK_EINVAL;
    if (!vmm_user_range_ok(cur_as(), buf, len, false))
        return ZK_EFAULT;

    size_t done = 0;
    while (done < len) {
        size_t n = len - done < WRITE_CHUNK ? len - done : WRITE_CHUNK;
        kprintf("%.*s", (int)n, (const char *)(buf + done));
        done += n;
    }
    return (int64_t)done;
}

static int64_t sys_read(int fd, uint64_t buf, size_t len)
{
    if (fd != 0)
        return ZK_EINVAL;
    if (len == 0)
        return 0;
    if (!vmm_user_range_ok(cur_as(), buf, len, true))
        return ZK_EFAULT;

    char *dst = (char *)buf;
    size_t n = 0;
    while (n == 0) {
        int c = keyboard_getc();
        if (c < 0x100)
            dst[n++] = (char)c;
    }
    int c;
    while (n < len && (c = keyboard_trygetc()) >= 0)
        if (c < 0x100)
            dst[n++] = (char)c;
    return (int64_t)n;
}

static int64_t sys_alloc_pages(size_t count)
{
    struct address_space *as = cur_as();
    if (count == 0 || count > MAX_ALLOC_PAGES)
        return ZK_EINVAL;

    uint64_t start = as->heap_top;
    for (size_t i = 0; i < count; i++) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p || !vmm_map_user(as, start + i * PAGE_SIZE, p, PTE_WRITE | PTE_NX)) {
            if (p)
                pmm_free(p);
            // pages mapped so far stay with the process and get freed on exit
            as->heap_top = start + i * PAGE_SIZE;
            return ZK_ENOMEM;
        }
    }
    as->heap_top = start + count * PAGE_SIZE;
    return (int64_t)start;
}

static void syscall_handler(struct interrupt_frame *f)
{
    // syscalls can take a while, let interrupts in
    sti();

    int64_t ret;
    switch (f->rax) {
    case SYS_EXIT:
        thread_exit((int)f->rdi);
    case SYS_WRITE:
        ret = sys_write((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_READ:
        ret = sys_read((int)f->rdi, f->rsi, f->rdx);
        break;
    case SYS_GETPID:
        ret = (int64_t)thread_current()->id;
        break;
    case SYS_SLEEP_MS:
        thread_sleep_ms(f->rdi);
        ret = 0;
        break;
    case SYS_YIELD:
        thread_yield();
        ret = 0;
        break;
    case SYS_UPTIME_MS:
        ret = (int64_t)timer_uptime_ms();
        break;
    case SYS_ALLOC_PAGES:
        ret = sys_alloc_pages(f->rdi);
        break;
    default:
        ret = ZK_ENOSYS;
        break;
    }

    f->rax = (uint64_t)ret;
    cli();
}

void syscall_init(void)
{
    idt_set_handler(ZK_SYSCALL_VECTOR, syscall_handler);
    idt_allow_user(ZK_SYSCALL_VECTOR);
}
