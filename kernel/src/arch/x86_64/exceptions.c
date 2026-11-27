#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "kprintf.h"
#include "panic.h"

static const struct {
    const char *mnemonic;
    const char *name;
} exceptions[32] = {
    [0]  = { "#DE", "Divide Error" },
    [1]  = { "#DB", "Debug" },
    [2]  = { "NMI", "Non-Maskable Interrupt" },
    [3]  = { "#BP", "Breakpoint" },
    [4]  = { "#OF", "Overflow" },
    [5]  = { "#BR", "Bound Range Exceeded" },
    [6]  = { "#UD", "Invalid Opcode" },
    [7]  = { "#NM", "Device Not Available" },
    [8]  = { "#DF", "Double Fault" },
    [9]  = { "---", "Coprocessor Segment Overrun" },
    [10] = { "#TS", "Invalid TSS" },
    [11] = { "#NP", "Segment Not Present" },
    [12] = { "#SS", "Stack-Segment Fault" },
    [13] = { "#GP", "General Protection Fault" },
    [14] = { "#PF", "Page Fault" },
    [15] = { "---", "Reserved" },
    [16] = { "#MF", "x87 Floating-Point Exception" },
    [17] = { "#AC", "Alignment Check" },
    [18] = { "#MC", "Machine Check" },
    [19] = { "#XM", "SIMD Floating-Point Exception" },
    [20] = { "#VE", "Virtualization Exception" },
    [21] = { "#CP", "Control Protection Exception" },
    [28] = { "#HV", "Hypervisor Injection Exception" },
    [29] = { "#VC", "VMM Communication Exception" },
    [30] = { "#SX", "Security Exception" },
};

static void explain_page_fault(uint64_t err)
{
    /* P W U RSVD I */
    kprintf("  faulting address (CR2): %p\n", (void *)read_cr2());
    kprintf("  cause: %s %s %s mode%s\n",
            (err & 0x10) ? "instruction fetch from" : (err & 0x2) ? "write to" : "read from",
            (err & 0x1) ? "protected page in" : "non-present page in",
            (err & 0x4) ? "user" : "kernel",
            (err & 0x8) ? ", reserved bit set in page table" : "");
}

static void explain_selector_error(uint64_t err)
{
    if (err == 0)
        return;
    static const char *tables[] = { "GDT", "IDT", "LDT", "IDT" };
    kprintf("  selector: index %lu in %s%s\n", (err >> 3) & 0x1FFF,
            tables[(err >> 1) & 3], (err & 1) ? " (external event)" : "");
}

static void dump_registers(const struct interrupt_frame *f)
{
    kprintf("  RIP %p  CS  %04lx  RFLAGS %016lx\n", (void *)f->rip, f->cs, f->rflags);
    kprintf("  RSP %p  SS  %04lx\n", (void *)f->rsp, f->ss);
    kprintf("  RAX %016lx  RBX %016lx  RCX %016lx\n", f->rax, f->rbx, f->rcx);
    kprintf("  RDX %016lx  RSI %016lx  RDI %016lx\n", f->rdx, f->rsi, f->rdi);
    kprintf("  RBP %016lx  R8  %016lx  R9  %016lx\n", f->rbp, f->r8, f->r9);
    kprintf("  R10 %016lx  R11 %016lx  R12 %016lx\n", f->r10, f->r11, f->r12);
    kprintf("  R13 %016lx  R14 %016lx  R15 %016lx\n", f->r13, f->r14, f->r15);
    kprintf("  CR0 %016lx  CR2 %016lx  CR3 %016lx\n", read_cr0(), read_cr2(), read_cr3());
    kprintf("  CR4 %016lx\n", read_cr4());
}

void exception_handle(struct interrupt_frame *f)
{
    uint64_t vec = f->vector;
    const char *mn = exceptions[vec].mnemonic ? exceptions[vec].mnemonic : "---";
    const char *name = exceptions[vec].name ? exceptions[vec].name : "Reserved";

    /* #DB and #BP aren't fatal */
    if (vec == 3 || vec == 1) {
        pr_info("%s %s at %p\n", mn, name, (void *)f->rip);
        return;
    }

    panic_begin();
    kprintf("\e[1;31m%s %s\e[0m (vector %lu, error code %#lx)\n", name, mn, vec, f->error_code);

    if (vec == 14)
        explain_page_fault(f->error_code);
    else if (vec == 10 || vec == 11 || vec == 12 || vec == 13)
        explain_selector_error(f->error_code);

    kprintf("  in %s mode\n", (f->cs & 3) ? "user" : "kernel");
    dump_registers(f);

    kprintf("  fault at:\n    %p\n", (void *)f->rip);
    print_backtrace(f->rbp);
    kprintf("  System halted.\n");
    hcf();
}
