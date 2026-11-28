#pragma once
#include <stddef.h>
#include <stdint.h>

struct limine_memmap_response;

void pmm_init(struct limine_memmap_response *memmap);

// hand bootloader memory to the allocator. nothing limine gave us may be
// used afterwards, including the memmap itself. returns pages reclaimed
size_t pmm_reclaim_bootloader(struct limine_memmap_response *memmap);

// return a physical address, 0 when out of memory
uint64_t pmm_alloc(void);
uint64_t pmm_alloc_zeroed(void);
uint64_t pmm_alloc_pages(size_t count);    // physically contiguous

// drops one reference, the page is freed with the last one
void pmm_free(uint64_t phys);
void pmm_ref(uint64_t phys);
unsigned pmm_refcount(uint64_t phys);
void pmm_free_pages(uint64_t phys, size_t count);

size_t pmm_total_pages(void);
size_t pmm_free_count(void);
// zeroes every page of ram except the ones in skip (sorted), allocations
// included. the allocator is gone afterwards, it's for the last moments
void pmm_wipe(const uint64_t *skip, size_t nskip);
