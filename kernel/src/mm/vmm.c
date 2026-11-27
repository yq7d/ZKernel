#include "mm/vmm.h"
#include "mm/heap.h"
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

static inline uint64_t read_cr3_phys(void)
{
    return read_cr3() & PTE_ADDR_MASK;
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

static bool map_page(uint64_t *pml4, uint64_t virt, uint64_t phys, uint64_t flags, bool huge)
{
    uint64_t *pdpt = next_level(pml4, (virt >> 39) & 511, true, flags);
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

// returns the leaf entry, or 0
static uint64_t walk(uint64_t *pml4, uint64_t virt, uint64_t *phys)
{
    uint64_t *t = pml4;
    for (int shift = 39; shift >= 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511];
        if (!(e & PTE_PRESENT))
            return 0;
        if (shift == 12 || (e & PTE_HUGE)) {
            uint64_t page_mask = (1UL << shift) - 1;
            if (phys)
                *phys = (e & PTE_ADDR_MASK & ~page_mask) | (virt & page_mask);
            return e;
        }
        t = phys_to_virt(e & PTE_ADDR_MASK);
    }
    return 0;
}

static uint64_t *pte_of(uint64_t *pml4, uint64_t virt)
{
    uint64_t *t = pml4;
    for (int shift = 39; shift > 12; shift -= 9) {
        uint64_t e = t[(virt >> shift) & 511];
        if (!(e & PTE_PRESENT) || (e & PTE_HUGE))
            return NULL;
        t = phys_to_virt(e & PTE_ADDR_MASK);
    }
    return &t[(virt >> 12) & 511];
}

bool vmm_translate(uint64_t virt, uint64_t *phys)
{
    return walk(kernel_pml4, virt, phys) != 0;
}

bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    bool ok = map_page(kernel_pml4, virt, phys, flags, false);
    if (ok)
        invlpg(virt);
    spin_unlock_irqrestore(&lock, irq);
    return ok;
}

void vmm_unmap(uint64_t virt)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *pte = pte_of(kernel_pml4, virt);
    if (pte) {
        *pte = 0;
        invlpg(virt);
    }
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
            if (!map_page(kernel_pml4, virt, phys, flags, true))
                panic("vmm: out of memory mapping hhdm");
            phys += HUGE_SIZE;
            continue;
        }
        if (!vmm_translate(virt, NULL) && !map_page(kernel_pml4, virt, phys, flags, false))
            panic("vmm: out of memory mapping hhdm");
        phys += PAGE_SIZE;
    }
}

static void map_kernel_range(char *start, char *end, uint64_t flags,
                             uint64_t kernel_phys, uint64_t kernel_virt)
{
    for (uint64_t v = ALIGN_DOWN((uint64_t)start, PAGE_SIZE);
         v < ALIGN_UP((uint64_t)end, PAGE_SIZE); v += PAGE_SIZE) {
        if (!map_page(kernel_pml4, v, v - kernel_virt + kernel_phys, flags, false))
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

    // fill the whole upper half now so every address space can share it
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

// the virtual range isn't reused, there's plenty of it
void vmm_free_stack(uint64_t top, size_t pages)
{
    for (uint64_t v = top - pages * PAGE_SIZE; v < top; v += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_translate(v, &phys)) {
            vmm_unmap(v);
            pmm_free(phys);
        }
    }
}

bool vmm_is_guard_page(uint64_t addr)
{
    return addr >= KSTACK_BASE && addr < next_stack && !vmm_translate(addr, NULL);
}

struct address_space *vmm_create_space(void)
{
    struct address_space *as = kzalloc(sizeof(*as));
    if (!as)
        return NULL;
    as->pml4_phys = pmm_alloc_zeroed();
    if (!as->pml4_phys) {
        kfree(as);
        return NULL;
    }
    as->pml4 = phys_to_virt(as->pml4_phys);
    // the upper half tables are shared, so kernel mappings show up everywhere
    memcpy(&as->pml4[256], &kernel_pml4[256], 256 * sizeof(uint64_t));
    return as;
}

static void free_table(uint64_t *table, int level)
{
    for (size_t i = 0; i < 512; i++) {
        uint64_t e = table[i];
        if (!(e & PTE_PRESENT))
            continue;
        uint64_t phys = e & PTE_ADDR_MASK;
        if (level > 1 && !(e & PTE_HUGE))
            free_table(phys_to_virt(phys), level - 1);
        else
            pmm_free(phys);    // leaf: a page owned by the space
    }
    pmm_free(hhdm_to_phys(table));
}

void vmm_destroy_space(struct address_space *as)
{
    if (!as)
        return;
    if (read_cr3_phys() == as->pml4_phys)
        vmm_activate(NULL);

    for (size_t i = 0; i < 256; i++) {
        uint64_t e = as->pml4[i];
        if (e & PTE_PRESENT)
            free_table(phys_to_virt(e & PTE_ADDR_MASK), 3);
    }
    pmm_free(as->pml4_phys);
    kfree(as);
}

bool vmm_map_user(struct address_space *as, uint64_t virt, uint64_t phys, uint64_t flags)
{
    if (virt >= USER_LIMIT)
        return false;
    uint64_t irq = spin_lock_irqsave(&lock);
    bool ok = map_page(as->pml4, virt, phys, flags | PTE_USER, false);
    if (ok && read_cr3_phys() == as->pml4_phys)
        invlpg(virt);
    spin_unlock_irqrestore(&lock, irq);
    return ok;
}

bool vmm_translate_user(struct address_space *as, uint64_t virt, uint64_t *phys)
{
    return virt < USER_LIMIT && walk(as->pml4, virt, phys) != 0;
}

bool vmm_user_range_ok(struct address_space *as, uint64_t addr, size_t len, bool write)
{
    if (!as || addr >= USER_LIMIT || len > USER_LIMIT - addr)
        return false;
    for (uint64_t p = ALIGN_DOWN(addr, PAGE_SIZE); p < addr + len; p += PAGE_SIZE) {
        uint64_t e = walk(as->pml4, p, NULL);
        if (!(e & PTE_USER) || (write && !(e & PTE_WRITE)))
            return false;
    }
    return true;
}

void vmm_activate(struct address_space *as)
{
    uint64_t phys = as ? as->pml4_phys : kernel_pml4_phys;
    if (read_cr3_phys() != phys)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}
