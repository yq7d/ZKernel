#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/gdt.h"
#include "arch/x86_64/pic.h"
#include "kprintf.h"
#include "proc/proc.h"
#include "sched/sched.h"

struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct idtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// present, dpl 0, interrupt gate (clears IF)
#define IDT_INTERRUPT_GATE 0x8E

static struct idt_entry idt[256];
static interrupt_handler_t handlers[256];

extern const uint64_t isr_stub_table[256];

static void idt_set_gate(uint8_t vec, uint64_t addr, uint8_t type_attr, uint8_t ist)
{
    idt[vec] = (struct idt_entry){
        .offset_low  = addr & 0xFFFF,
        .selector    = GDT_KERNEL_CODE,
        .ist         = ist,
        .type_attr   = type_attr,
        .offset_mid  = (addr >> 16) & 0xFFFF,
        .offset_high = addr >> 32,
    };
}

void idt_init(void)
{
    for (int v = 0; v < 256; v++)
        idt_set_gate((uint8_t)v, isr_stub_table[v], IDT_INTERRUPT_GATE, 0);

    // own stacks so these still work if the kernel stack is bad
    idt[8].ist  = IST_DOUBLE_FAULT;
    idt[2].ist  = IST_NMI;
    idt[18].ist = IST_MACHINE_CHECK;

    struct idtr idtr = { sizeof(idt) - 1, (uint64_t)idt };
    __asm__ volatile ("lidt %0" : : "m"(idtr));
}

// lets ring 3 use `int vector`
void idt_allow_user(uint8_t vector)
{
    idt[vector].type_attr = IDT_INTERRUPT_GATE | (3 << 5);
}

void idt_set_handler(uint8_t vector, interrupt_handler_t handler)
{
    handlers[vector] = handler;
}

void irq_set_handler(uint8_t irq, interrupt_handler_t handler)
{
    handlers[IRQ_BASE + irq] = handler;
    pic_unmask(irq);
}

void interrupt_dispatch(struct interrupt_frame *frame)
{
    uint64_t vec = frame->vector;

    if (vec < 32) {
        if (handlers[vec])
            handlers[vec](frame);
        else
            exception_handle(frame);
        return;
    }

    if (vec < IRQ_BASE + 16) {
        uint8_t irq = (uint8_t)(vec - IRQ_BASE);
        if (pic_is_spurious(irq))
            return;
        if (handlers[vec])
            handlers[vec](frame);
        else
            pr_warn("unhandled IRQ %u\n", irq);
        pic_eoi(irq);
        // after the eoi, or the pic would hold back the next tick
        sched_preempt();
    } else {
        if (handlers[vec])
            handlers[vec](frame);
        else
            pr_warn("unhandled interrupt vector %lu\n", vec);
        sched_preempt();
    }

    if ((frame->cs & 3) == 3)
        proc_check_killed();
}
