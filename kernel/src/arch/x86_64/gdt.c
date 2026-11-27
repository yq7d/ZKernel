#include "arch/x86_64/gdt.h"
#include <stddef.h>

struct tss {
    uint32_t reserved0;
    uint64_t rsp[3];
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

struct gdtr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

/* access byte */
#define SEG_PRESENT   0x80
#define SEG_DPL(n)    ((n) << 5)
#define SEG_CODE_DATA 0x10
#define SEG_EXEC      0x08
#define SEG_RW        0x02
#define SEG_TSS_AVAIL 0x09

/* flags */
#define SEG_GRAN_4K   0x8
#define SEG_32BIT     0x4
#define SEG_LONG      0x2

#define STACK_SIZE 16384

static uint64_t gdt[7];
static struct tss tss;

static uint8_t ist_stacks[3][STACK_SIZE] __attribute__((aligned(16)));
static uint8_t rsp0_stack[STACK_SIZE] __attribute__((aligned(16)));

extern void gdt_load(const struct gdtr *gdtr, uint16_t code_sel, uint16_t data_sel);

/* base/limit are ignored in long mode, filled in anyway */
static uint64_t make_segment(uint8_t access, uint8_t flags)
{
    return 0xFFFFULL
         | ((uint64_t)access << 40)
         | (0xFULL << 48)
         | ((uint64_t)flags << 52);
}

void gdt_init(void)
{
    gdt[0] = 0;
    gdt[1] = make_segment(SEG_PRESENT | SEG_DPL(0) | SEG_CODE_DATA | SEG_EXEC | SEG_RW,
                          SEG_GRAN_4K | SEG_LONG);
    gdt[2] = make_segment(SEG_PRESENT | SEG_DPL(0) | SEG_CODE_DATA | SEG_RW,
                          SEG_GRAN_4K | SEG_32BIT);
    gdt[3] = make_segment(SEG_PRESENT | SEG_DPL(3) | SEG_CODE_DATA | SEG_RW,
                          SEG_GRAN_4K | SEG_32BIT);
    gdt[4] = make_segment(SEG_PRESENT | SEG_DPL(3) | SEG_CODE_DATA | SEG_EXEC | SEG_RW,
                          SEG_GRAN_4K | SEG_LONG);

    for (int i = 0; i < 3; i++)
        tss.ist[i] = (uint64_t)&ist_stacks[i][STACK_SIZE];
    tss.rsp[0] = (uint64_t)&rsp0_stack[STACK_SIZE];
    tss.iopb_offset = sizeof(tss);

    /* tss descriptor takes two slots */
    uint64_t base = (uint64_t)&tss;
    uint64_t limit = sizeof(tss) - 1;
    gdt[5] = (limit & 0xFFFF)
           | ((base & 0xFFFFFF) << 16)
           | ((uint64_t)(SEG_PRESENT | SEG_TSS_AVAIL) << 40)
           | (((limit >> 16) & 0xF) << 48)
           | (((base >> 24) & 0xFF) << 56);
    gdt[6] = base >> 32;

    struct gdtr gdtr = { sizeof(gdt) - 1, (uint64_t)gdt };
    gdt_load(&gdtr, GDT_KERNEL_CODE, GDT_KERNEL_DATA);

    __asm__ volatile ("ltr %0" : : "r"((uint16_t)GDT_TSS));
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss.rsp[0] = rsp0;
}
