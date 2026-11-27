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
#define PTE_NX      (1UL << 63)

void vmm_init(struct limine_memmap_response *memmap,
              uint64_t kernel_phys, uint64_t kernel_virt);

// 4k pages in the kernel address space. flags are PTE_* (present is implied)
bool vmm_map(uint64_t virt, uint64_t phys, uint64_t flags);
void vmm_unmap(uint64_t virt);
bool vmm_translate(uint64_t virt, uint64_t *phys);

// returns the top of a new stack, with an unmapped guard page under it
uint64_t vmm_alloc_stack(size_t pages);
bool vmm_is_guard_page(uint64_t addr);
