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
#define EFER_NXE      (1UL << 11)
#define CR0_WP        (1UL << 16)

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

    uint64_t cr0 = read_cr0();
    if (!(cr0 & CR0_WP))
        __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0 | CR0_WP));

    uint64_t efer = rdmsr(MSR_EFER);
    if (!(efer & EFER_NXE))
        wrmsr(MSR_EFER, efer | EFER_NXE);

    __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_pml4_phys) : "memory");
}

// freed stack ranges get reused, so their page tables don't pile up
struct stack_slot {
    uint64_t top;
    size_t pages;
    struct stack_slot *next;
};
static struct stack_slot *free_stacks;

static uint64_t reuse_stack(size_t pages)
{
    uint64_t irq = spin_lock_irqsave(&stack_lock);
    struct stack_slot *s = NULL;
    for (struct stack_slot **p = &free_stacks; *p; p = &(*p)->next) {
        if ((*p)->pages == pages) {
            s = *p;
            *p = s->next;
            break;
        }
    }
    spin_unlock_irqrestore(&stack_lock, irq);
    if (!s)
        return 0;
    uint64_t top = s->top;
    kfree(s);
    return top;
}

void *vmm_map_mmio(uint64_t phys, size_t size)
{
    uint64_t start = ALIGN_DOWN(phys, PAGE_SIZE), end = ALIGN_UP(phys + size, PAGE_SIZE);
    for (uint64_t p = start; p < end; p += PAGE_SIZE) {
        uint64_t virt = p + hhdm_offset;
        if (!vmm_translate(virt, NULL) && !vmm_map(virt, p, PTE_WRITE | PTE_NX | PTE_PCD | PTE_PWT))
            return NULL;
    }
    return phys_to_virt(phys);
}

uint64_t vmm_alloc_stack(size_t pages)
{
    uint64_t top = reuse_stack(pages);
    if (top) {
        for (uint64_t v = top - pages * PAGE_SIZE; v < top; v += PAGE_SIZE) {
            uint64_t p = pmm_alloc_zeroed();
            if (!p || !vmm_map(v, p, PTE_WRITE | PTE_NX))
                panic("vmm: out of memory allocating a stack");
        }
        return top;
    }

    uint64_t irq = spin_lock_irqsave(&stack_lock);
    uint64_t guard = next_stack;
    uint64_t bottom = guard + PAGE_SIZE;
    top = bottom + pages * PAGE_SIZE;
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

void vmm_free_stack(uint64_t top, size_t pages)
{
    for (uint64_t v = top - pages * PAGE_SIZE; v < top; v += PAGE_SIZE) {
        uint64_t phys;
        if (vmm_translate(v, &phys)) {
            vmm_unmap(v);
            pmm_free(phys);
        }
    }
    struct stack_slot *s = kmalloc(sizeof(*s));
    if (!s)
        return;    // the range is just lost
    s->top = top;
    s->pages = pages;
    uint64_t irq = spin_lock_irqsave(&stack_lock);
    s->next = free_stacks;
    free_stacks = s;
    spin_unlock_irqrestore(&stack_lock, irq);
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
        else if (!(e & PTE_NOFREE))
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

void vmm_free_user(struct address_space *as, uint64_t virt)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *pte = pte_of(as->pml4, virt);
    uint64_t e = pte ? *pte : 0;
    if (e & PTE_PRESENT) {
        *pte = 0;
        if (read_cr3_phys() == as->pml4_phys)
            invlpg(virt);
    }
    spin_unlock_irqrestore(&lock, irq);
    if ((e & PTE_PRESENT) && !(e & PTE_NOFREE))
        pmm_free(e & PTE_ADDR_MASK);
}

bool vmm_protect_user(struct address_space *as, uint64_t virt, uint64_t flags)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *pte = pte_of(as->pml4, virt);
    bool ok = pte && (*pte & PTE_PRESENT);
    if (ok) {
        uint64_t e = *pte & ~(PTE_NX | PTE_WRITE);
        e |= flags & PTE_NX;
        // shared copy on write pages stay read only until a write copies
        // them, the cow bit decides whether that write is allowed at all
        if (flags & PTE_WRITE) {
            if (pmm_refcount(e & PTE_ADDR_MASK) > 1 && !(e & (PTE_NOFREE | PTE_SHARED)))
                e |= PTE_COW;
            else
                e = (e & ~PTE_COW) | PTE_WRITE;
        } else {
            e &= ~PTE_COW;
        }
        *pte = e;
        if (read_cr3_phys() == as->pml4_phys)
            invlpg(virt);
    }
    spin_unlock_irqrestore(&lock, irq);
    return ok;
}

bool vmm_grow_stack(struct address_space *as, uint64_t addr)
{
    if (!as || addr < as->stack_low || addr >= as->stack_top)
        return false;
    uint64_t v = ALIGN_DOWN(addr, PAGE_SIZE);
    if (vmm_translate_user(as, v, NULL))
        return false;
    uint64_t p = pmm_alloc_zeroed();
    if (!p)
        return false;
    if (!vmm_map_user(as, v, p, PTE_WRITE | PTE_NX)) {
        pmm_free(p);
        return false;
    }
    return true;
}

void vmm_unmap_user(struct address_space *as, uint64_t virt)
{
    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *pte = pte_of(as->pml4, virt);
    if (pte) {
        *pte = 0;
        if (read_cr3_phys() == as->pml4_phys)
            invlpg(virt);
    }
    spin_unlock_irqrestore(&lock, irq);
}

bool vmm_translate_user(struct address_space *as, uint64_t virt, uint64_t *phys)
{
    return virt < USER_LIMIT && walk(as->pml4, virt, phys) != 0;
}

// copies of a shared page are made before the kernel writes into it
bool vmm_user_range_ok(struct address_space *as, uint64_t addr, size_t len, bool write)
{
    if (!as || addr >= USER_LIMIT || len > USER_LIMIT - addr)
        return false;
    for (uint64_t p = ALIGN_DOWN(addr, PAGE_SIZE); p < addr + len; p += PAGE_SIZE) {
        uint64_t e = walk(as->pml4, p, NULL);
        if (!(e & PTE_PRESENT) && vmm_grow_stack(as, p))
            e = walk(as->pml4, p, NULL);
        if (!(e & PTE_USER))
            return false;
        if (write && !(e & PTE_WRITE) && !vmm_cow_fault(as, p))
            return false;
    }
    return true;
}

static void flush_space(struct address_space *as)
{
    if (read_cr3_phys() == as->pml4_phys)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(as->pml4_phys) : "memory");
}

// recursively copies one table level. leaves get shared and write protected
static bool clone_table(uint64_t *src, uint64_t *dst, int level)
{
    for (size_t i = 0; i < 512; i++) {
        uint64_t e = src[i];
        if (!(e & PTE_PRESENT))
            continue;
        if (level == 1) {
            if (e & PTE_NOFREE) {
                dst[i] = e;    // device memory stays shared and writable
                continue;
            }
            if (e & PTE_SHARED) {
                pmm_ref(e & PTE_ADDR_MASK);
                dst[i] = e;
                continue;
            }
            if (e & (PTE_WRITE | PTE_COW)) {
                e = (e & ~PTE_WRITE) | PTE_COW;
                src[i] = e;
            }
            pmm_ref(e & PTE_ADDR_MASK);
            dst[i] = e;
            continue;
        }
        uint64_t p = pmm_alloc_zeroed();
        if (!p)
            return false;
        dst[i] = p | (e & ~PTE_ADDR_MASK);
        if (!clone_table(phys_to_virt(e & PTE_ADDR_MASK), phys_to_virt(p), level - 1))
            return false;
    }
    return true;
}

struct address_space *vmm_clone_space(struct address_space *src)
{
    struct address_space *as = vmm_create_space();
    if (!as)
        return NULL;
    as->heap_top = src->heap_top;
    as->brk_start = src->brk_start;
    as->brk = src->brk;
    as->mmap_next = src->mmap_next;
    as->stack_low = src->stack_low;
    as->stack_top = src->stack_top;

    uint64_t irq = spin_lock_irqsave(&lock);
    bool ok = true;
    for (size_t i = 0; i < 256 && ok; i++) {
        uint64_t e = src->pml4[i];
        if (!(e & PTE_PRESENT))
            continue;
        uint64_t p = pmm_alloc_zeroed();
        if (!p) {
            ok = false;
            break;
        }
        as->pml4[i] = p | (e & ~PTE_ADDR_MASK);
        ok = clone_table(phys_to_virt(e & PTE_ADDR_MASK), phys_to_virt(p), 3);
    }
    flush_space(src);
    spin_unlock_irqrestore(&lock, irq);
    if (!ok) {
        vmm_destroy_space(as);    // frees whatever got copied
        return NULL;
    }
    return as;
}

bool vmm_cow_fault(struct address_space *as, uint64_t addr)
{
    if (!as || addr >= USER_LIMIT)
        return false;
    uint64_t virt = ALIGN_DOWN(addr, PAGE_SIZE);

    uint64_t irq = spin_lock_irqsave(&lock);
    uint64_t *pte = pte_of(as->pml4, virt);
    if (!pte || !(*pte & PTE_PRESENT) || !(*pte & PTE_COW)) {
        spin_unlock_irqrestore(&lock, irq);
        return false;
    }
    uint64_t old = *pte & PTE_ADDR_MASK;
    uint64_t flags = (*pte & ~PTE_ADDR_MASK & ~PTE_COW) | PTE_WRITE;

    if (pmm_refcount(old) > 1) {
        uint64_t p = pmm_alloc();
        if (!p) {
            spin_unlock_irqrestore(&lock, irq);
            return false;
        }
        memcpy(phys_to_virt(p), phys_to_virt(old), PAGE_SIZE);
        *pte = p | flags;
        pmm_free(old);
    } else {
        *pte = old | flags;    // last user, just take it
    }
    if (read_cr3_phys() == as->pml4_phys)
        invlpg(virt);
    spin_unlock_irqrestore(&lock, irq);
    return true;
}

void vmm_activate(struct address_space *as)
{
    uint64_t phys = as ? as->pml4_phys : kernel_pml4_phys;
    if (read_cr3_phys() != phys)
        __asm__ volatile ("mov %0, %%cr3" : : "r"(phys) : "memory");
}
