#pragma once
#include <stdint.h>

// user data comes before user code so sysret works
#define GDT_KERNEL_CODE 0x08
#define GDT_KERNEL_DATA 0x10
#define GDT_USER_DATA   (0x18 | 3)
#define GDT_USER_CODE   (0x20 | 3)
#define GDT_TSS         0x28

// IST slots, 1-based like the IDT field
#define IST_DOUBLE_FAULT  1
#define IST_NMI           2
#define IST_MACHINE_CHECK 3

void gdt_init(void);
void tss_set_kernel_stack(uint64_t rsp0);
