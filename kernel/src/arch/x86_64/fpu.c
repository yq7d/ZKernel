#include "arch/x86_64/fpu.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"

#define CR0_MP (1UL << 1)
#define CR0_EM (1UL << 2)
#define CR0_TS (1UL << 3)
#define CR0_NE (1UL << 5)
#define CR4_OSFXSR     (1UL << 9)
#define CR4_OSXMMEXCPT (1UL << 10)

// clean state every new thread starts from
static struct fpu_state initial __attribute__((aligned(16)));

void fpu_init(void)
{
    uint64_t cr0 = read_cr0();
    cr0 &= ~(CR0_EM | CR0_TS);
    cr0 |= CR0_MP | CR0_NE;
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    uint64_t cr4 = read_cr4();
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit");
    uint32_t mxcsr = 0x1f80;    // all exceptions masked
    __asm__ volatile ("ldmxcsr %0" : : "m"(mxcsr));
    fpu_save(&initial);
}

void fpu_init_state(struct fpu_state *s)
{
    memcpy(s, &initial, sizeof(*s));
}

void fpu_save(struct fpu_state *s)
{
    __asm__ volatile ("fxsave64 (%0)" : : "r"(s) : "memory");
}

void fpu_restore(const struct fpu_state *s)
{
    __asm__ volatile ("fxrstor64 (%0)" : : "r"(s) : "memory");
}
