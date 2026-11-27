#pragma once
#include <stdint.h>

/* 0-31 exceptions, 32-47 pic irqs */
#define IRQ_BASE 32

/* matches the push order in isr.asm */
struct interrupt_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

typedef void (*interrupt_handler_t)(struct interrupt_frame *frame);

void idt_init(void);
void idt_set_handler(uint8_t vector, interrupt_handler_t handler);
void irq_set_handler(uint8_t irq, interrupt_handler_t handler);

void exception_handle(struct interrupt_frame *frame);
