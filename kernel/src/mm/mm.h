#pragma once
#include <stddef.h>
#include <stdint.h>

#define PAGE_SIZE  4096UL
#define PAGE_SHIFT 12

#define ALIGN_UP(x, a)   (((x) + (a) - 1) & ~((uint64_t)(a) - 1))
#define ALIGN_DOWN(x, a) ((x) & ~((uint64_t)(a) - 1))

// virtual layout (higher half)
//   ffff800000000000  hhdm, all physical memory
//   ffffc00000000000  kernel heap
//   ffffe00000000000  kernel stacks, each with a guard page below
//   ffffffff80000000  kernel image
#define HEAP_BASE   0xffffc00000000000UL
#define HEAP_LIMIT  0xffffd00000000000UL
#define KSTACK_BASE 0xffffe00000000000UL
#define KSTACK_LIMIT 0xfffff00000000000UL

// user space: everything below this
#define USER_LIMIT  0x0000800000000000UL

extern uint64_t hhdm_offset;

static inline void *phys_to_virt(uint64_t phys)
{
    return (void *)(phys + hhdm_offset);
}

static inline uint64_t hhdm_to_phys(const void *virt)
{
    return (uint64_t)virt - hhdm_offset;
}
