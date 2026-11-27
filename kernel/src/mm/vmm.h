#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

#define PTE_PRESENT (1UL << 0)
#define PTE_WRITE   (1UL << 1)
#define PTE_USER    (1UL << 2)
#define PTE_PWT     (1UL << 3)
#define PTE_PCD     (1UL << 4)
#define PTE_HUGE    (1UL << 7)
#define PTE_GLOBAL  (1UL << 8)
#define PTE_COW     (1UL << 9)    // available bit: read only until written, then copied
#define PTE_NX      (1UL << 63)

void vmm_init(struct limine_memmap_response *memmap,
              uint64_t kernel_phys, uint64_t kernel_virt);

// 4k pages in the kernel address space. flags are PTE_* (present is implied)
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
bool vmm_translate(uint64_t virt, uint64_t *phys);

// returns the top of a new stack, with an unmapped guard page under it
uint64_t vmm_alloc_stack(size_t pages);
void vmm_free_stack(uint64_t top, size_t pages);
bool vmm_is_guard_page(uint64_t addr);

// user address spaces. the lower half is private, the upper half is shared
struct address_space {
    uint64_t pml4_phys;
    uint64_t *pml4;
    uint64_t heap_top;    // next address handed out by sys_alloc
};

struct address_space *vmm_create_space(void);
// frees every page mapped in the lower half
void vmm_destroy_space(struct address_space *as);
bool vmm_map_user(struct address_space *as, uint64_t virt, uint64_t phys, uint64_t flags);
bool vmm_translate_user(struct address_space *as, uint64_t virt, uint64_t *phys);
bool vmm_user_range_ok(struct address_space *as, uint64_t addr, size_t len, bool write);
// copy on write duplicate of the lower half, for fork
struct address_space *vmm_clone_space(struct address_space *src);
// resolves a write to a copy on write page. false if addr isn't one
bool vmm_cow_fault(struct address_space *as, uint64_t addr);
// NULL switches back to the kernel-only tables
void vmm_activate(struct address_space *as);
