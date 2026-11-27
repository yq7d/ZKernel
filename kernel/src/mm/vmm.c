#include "mm/vmm.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "arch/x86_64/cpu.h"
#include "lib/spinlock.h"
#include "lib/string.h"
#include "panic.h"
#include <limine.h>

#define PTE_ADDR_MASK 0x000ffffffffff000UL
#define HUGE_SIZE     0x200000UL
#define MSR_EFER      0xC0000080
#define EFER_NXE      (1UL << 11)

uint64_t hhdm_offset;

static uint64_t *kernel_pml4;
static uint64_t kernel_pml4_phys;
static uint64_t next_stack = KSTACK_BASE;
static spinlock_t lock = SPINLOCK_INIT;
static spinlock_t stack_lock = SPINLOCK_INIT;

// linker.ld
extern char __kernel_start[], __text_start[], __text_end[];
extern char __rodata_start[], __rodata_end[], __data_start[], __data_end[];

static inline void invlpg(uint64_t virt)
{
    __asm__ volatile ("invlpg (%0)" : : "r"(virt) : "memory");
}

static uint64_t *next_level(uint64_t *table, size_t index, bool create, uint64_t flags)
{
    uint64_t e = table[index];
    if (e & PTE_PRESENT) {
        if (e & PTE_HUGE)
            return NULL;
        if ((flags & PTE_USER) && !(e & PTE_USER))
            table[index] = e | PTE_USER;
        return phys_to_virt(e & PTE_ADDR_MASK);
    }
    if (!create)
        return NULL;

    uint64_t p = pmm_alloc_zeroed();
    if (!p)
        return NULL;
    // leaf entries decide the real permissions
    table[index] = p | PTE_PRESENT | PTE_WRITE | (flags & PTE_USER);
    return phys_to_virt(p);
}

static bool map_page(uint64_t virt, uint64_t phys, uint64_t flags, bool huge)
{
    uint64_t *pdpt = next_level(kernel_pml4, (virt >> 39) & 511, true, flags);
    if (!pdpt)
        return false;
    uint64_t *pd = next_level(pdpt, (virt >> 30) & 511, true, flags);
    if (!pd)
        return false;

    size_t i2 = (virt >> 21) & 511;
    if (huge) {
        pd[i2] = phys | flags | PTE_PRESENT | PTE_HUGE;
        return true;
    }

    uint64_t *pt = next_level(pd, i2, true, flags);
    if (!pt)
        return false;
    pt[(virt >> 12) & 511] = phys | flags | PTE_PRESENT;
    return true;
}

bool vmm_translate(uint64_t virt, uint64_t *phys)
{
    uint64_t *t = kernel_pml4;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511];
        if (!(e & PTE_PRESENT))
            return false;
        if (shift == 12 || (e & PTE_HUGE)) {
            uint64_t page_mask = (1UL << shift) - 1;
            if (phys)
                *phys = (e & PTE_ADDR_MASK & ~page_mask) | (virt & page_mask);
            return true;
        }
        t = phys_to_virt(e & PTE_ADDR_MASK);
    }
    return false;
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    bool ok = map_page(virt, phys, flags, false);
    if (ok)
        invlpg(virt);
    spin_unlock_irqrestore(&lock, irq);
    return ok;
}

void vmm_unmap(uint64_t virt)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *t = kernel_pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511];
        if (!(e & PTE_PRESENT) || (e & PTE_HUGE)) {
            spin_unlock_irqrestore(&lock, irq);
            return;
        }
        t = phys_to_virt(e & PTE_ADDR_MASK);
    }
    t[(virt >> 12) & 511] = 0;
    invlpg(virt);
    spin_unlock_irqrestore(&lock, irq);
}

// uses 2MiB pages where the range allows it
static void map_hhdm_range(uint64_t base, uint64_t length)
{
    uint64_t phys = ALIGN_DOWN(base, PAGE_SIZE);
    uint64_t end = ALIGN_UP(base + length, PAGE_SIZE);
    uint64_t flags = PTE_WRITE | PTE_NX;

    while (phys < end) {
        uint64_t virt = phys + hhdm_offset;
        if (phys % HUGE_SIZE == 0 && end - phys >= HUGE_SIZE) {
            if (!map_page(virt, phys, flags, true))
                panic("vmm: out of memory mapping hhdm");
            phys += HUGE_SIZE;
            continue;
        }
        if (!vmm_translate(virt, NULL) && !map_page(virt, phys, flags, false))
            panic("vmm: out of memory mapping hhdm");
        phys += PAGE_SIZE;
    }
}

static void map_kernel_range(char *start, char *end, uint64_t flags,
                             uint64_t kernel_phys, uint64_t kernel_virt)
{
    for (uint64_t v = ALIGN_DOWN((uint64_t)start, PAGE_SIZE);
         v < ALIGN_UP((uint64_t)end, PAGE_SIZE); v += PAGE_SIZE) {
        if (!map_page(v, v - kernel_virt + kernel_phys, flags, false))
            panic("vmm: out of memory mapping the kernel");
    }
}

void vmm_init(struct limine_memmap_response *memmap,
              uint64_t kernel_phys, uint64_t kernel_virt)
{
    kernel_pml4_phys = pmm_alloc_zeroed();
    if (!kernel_pml4_phys)
        panic("vmm: no memory for the pml4");
    kernel_pml4 = phys_to_virt(kernel_pml4_phys);

    // fill the whole upper half now so every address space can share it later
    for (size_t i = 256; i < 512; i++)
        if (!next_level(kernel_pml4, i, true, 0))
            panic("vmm: no memory for kernel page tables");

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_RESERVED || e->type == LIMINE_MEMMAP_BAD_MEMORY)
            continue;
        map_hhdm_range(e->base, e->length);
    }

    map_kernel_range(__kernel_start, __text_start, PTE_WRITE | PTE_NX, kernel_phys, kernel_virt);
    map_kernel_range(__text_start, __text_end, 0, kernel_phys, kernel_virt);
    map_kernel_range(__rodata_start, __rodata_end, PTE_NX, kernel_phys, kernel_virt);
    map_kernel_range(__data_start, __data_end, PTE_WRITE | PTE_NX, kernel_phys, kernel_virt);

    uint64_t efer = rdmsr(MSR_EFER);
    if (!(efer & EFER_NXE))
        wrmsr(MSR_EFER, efer | EFER_NXE);

    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

uint64_t vmm_alloc_stack(size_t pages)
{
    uint64_t irq = spin_lock_irqsave(&stack_lock);
    uint64_t guard = next_stack;
    uint64_t bottom = guard + PAGE_SIZE;
    uint64_t top = bottom + pages * PAGE_SIZE;
    if (top > KSTACK_LIMIT) {
        spin_unlock_irqrestore(&stack_lock, irq);
        return 0;
    }
    next_stack = top;
    spin_unlock_irqrestore(&stack_lock, irq);

    for (uint64_t v = bottom; v < top; v += PAGE_SIZE) {
        uint64_t p = pmm_alloc_zeroed();
        if (!p || !vmm_map(v, p, PTE_WRITE | PTE_NX))
            panic("vmm: out of memory allocating a stack");
    }
    return top;
}

bool vmm_is_guard_page(uint64_t addr)
{
    return addr >= KSTACK_BASE && addr < next_stack && !vmm_translate(addr, NULL);
}
