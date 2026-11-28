// bitmap allocator, one bit per 4k page, 1 = used
#include "mm/pmm.h"
#include "mm/mm.h"
#include "lib/spinlock.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "panic.h"
#include <limine.h>
#include <stdbool.h>

#define LOW_MEMORY_END 0x100000    // keep the first 1MiB for real mode stuff later

static uint8_t *bitmap;
static uint16_t *refs;             // per page, for pages shared copy on write
static size_t page_count;          // pages covered by the bitmap
static size_t total_pages;         // usable pages
static size_t free_pages;
static size_t next_hint;
static spinlock_t lock = SPINLOCK_INIT;

// every range of ram pages are handed out from, for pmm_wipe
#define MAX_RAM_RANGES 128
static struct { uint64_t base, end; } ram[MAX_RAM_RANGES];
static size_t nram;

static void note_ram(uint64_t base, uint64_t length)
{
    if (nram < MAX_RAM_RANGES)
        ram[nram++] = (typeof(ram[0])){ ALIGN_UP(base, PAGE_SIZE), ALIGN_DOWN(base + length, PAGE_SIZE) };
}

static inline bool page_used(size_t i)  { return bitmap[i / 8] & (1 << (i % 8)); }
static inline void mark_used(size_t i)  { bitmap[i / 8] |= (uint8_t)(1 << (i % 8)); }
static inline void mark_free(size_t i)  { bitmap[i / 8] &= (uint8_t)~(1 << (i % 8)); }

static void reserve_range(uint64_t base, uint64_t length)
{
    size_t first = base / PAGE_SIZE;
    size_t last = ALIGN_UP(base + length, PAGE_SIZE) / PAGE_SIZE;
    for (size_t i = first; i < last && i < page_count; i++) {
        if (!page_used(i)) {
            mark_used(i);
            free_pages--;
        }
    }
}

void pmm_init(struct limine_memmap_response *memmap)
{
    uint64_t top = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE ||
            e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE ||
            e->type == LIMINE_MEMMAP_EXECUTABLE_AND_MODULES) {
            if (e->base + e->length > top)
                top = e->base + e->length;
        }
    }

    page_count = top / PAGE_SIZE;
    for (uint64_t i = 0; i < memmap->entry_count; i++)
        if (memmap->entries[i]->type == LIMINE_MEMMAP_USABLE)
            note_ram(memmap->entries[i]->base, memmap->entries[i]->length);
    size_t bitmap_bytes = ALIGN_UP((page_count + 7) / 8, 16);
    size_t bitmap_size = ALIGN_UP(bitmap_bytes + page_count * sizeof(uint16_t), PAGE_SIZE);

    uint64_t bitmap_phys = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_USABLE && e->length >= bitmap_size &&
            e->base >= LOW_MEMORY_END) {
            bitmap_phys = e->base;
            break;
        }
    }
    if (!bitmap_phys)
        panic("pmm: no region big enough for the %zu byte bitmap", bitmap_size);

    bitmap = phys_to_virt(bitmap_phys);
    memset(bitmap, 0xFF, bitmap_bytes);
    refs = (uint16_t *)(bitmap + bitmap_bytes);
    memset(refs, 0, page_count * sizeof(uint16_t));

    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type != LIMINE_MEMMAP_USABLE)
            continue;
        size_t first = ALIGN_UP(e->base, PAGE_SIZE) / PAGE_SIZE;
        size_t last = ALIGN_DOWN(e->base + e->length, PAGE_SIZE) / PAGE_SIZE;
        for (size_t p = first; p < last; p++) {
            mark_free(p);
            free_pages++;
            total_pages++;
        }
    }

    reserve_range(bitmap_phys, bitmap_size);
    reserve_range(0, LOW_MEMORY_END);
    next_hint = LOW_MEMORY_END / PAGE_SIZE;
}

uint64_t pmm_alloc(void)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    uint64_t result = 0;
    for (size_t n = 0; n < page_count; n++) {
        size_t i = (next_hint + n) % page_count;
        if (bitmap[i / 8] == 0xFF) {
            n += 7 - (i % 8);    // skip the rest of a full byte
            continue;
        }
        if (!page_used(i)) {
            mark_used(i);
            refs[i] = 1;
            free_pages--;
            next_hint = i + 1;
            result = (uint64_t)i * PAGE_SIZE;
            break;
        }
    }
    spin_unlock_irqrestore(&lock, flags);
    return result;
}

uint64_t pmm_alloc_zeroed(void)
{
    uint64_t p = pmm_alloc();
    if (p)
        memset(phys_to_virt(p), 0, PAGE_SIZE);
    return p;
}

uint64_t pmm_alloc_pages(size_t count)
{
    if (count == 0)
        return 0;
    if (count == 1)
        return pmm_alloc();

    uint64_t flags = spin_lock_irqsave(&lock);
    uint64_t result = 0;
    size_t run = 0;
    for (size_t i = LOW_MEMORY_END / PAGE_SIZE; i < page_count; i++) {
        if (page_used(i)) {
            run = 0;
            continue;
        }
        if (++run == count) {
            size_t start = i + 1 - count;
            for (size_t p = start; p <= i; p++) {
                mark_used(p);
                refs[p] = 1;
            }
            free_pages -= count;
            result = (uint64_t)start * PAGE_SIZE;
            break;
        }
    }
    spin_unlock_irqrestore(&lock, flags);
    return result;
}

void pmm_free(uint64_t phys)
{
    size_t i = phys / PAGE_SIZE;
    if (phys % PAGE_SIZE || i >= page_count)
        panic("pmm: freeing bad address %#lx", phys);

    uint64_t flags = spin_lock_irqsave(&lock);
    if (!page_used(i)) {
        spin_unlock_irqrestore(&lock, flags);
        panic("pmm: double free of %#lx", phys);
    }
    if (refs[i] > 1) {
        refs[i]--;
        spin_unlock_irqrestore(&lock, flags);
        return;
    }
    refs[i] = 0;
    mark_free(i);
    free_pages++;
    if (i < next_hint)
        next_hint = i;
    spin_unlock_irqrestore(&lock, flags);
}

void pmm_free_pages(uint64_t phys, size_t count)
{
    for (size_t n = 0; n < count; n++)
        pmm_free(phys + n * PAGE_SIZE);
}

size_t pmm_reclaim_bootloader(struct limine_memmap_response *memmap)
{
    // copy the ranges first, the memmap lives in the memory being freed
    size_t n = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++)
        if (memmap->entries[i]->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE)
            n++;
    if (n == 0)
        return 0;

    struct range { uint64_t base, length; } *ranges = kmalloc(n * sizeof(*ranges));
    if (!ranges)
        return 0;
    size_t r = 0;
    for (uint64_t i = 0; i < memmap->entry_count; i++) {
        struct limine_memmap_entry *e = memmap->entries[i];
        if (e->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            ranges[r++] = (struct range){ e->base, e->length };
            note_ram(e->base, e->length);
        }
    }

    size_t reclaimed = 0;
    uint64_t flags = spin_lock_irqsave(&lock);
    for (size_t k = 0; k < n; k++) {
        size_t first = ALIGN_UP(ranges[k].base, PAGE_SIZE) / PAGE_SIZE;
        size_t last = ALIGN_DOWN(ranges[k].base + ranges[k].length, PAGE_SIZE) / PAGE_SIZE;
        for (size_t p = first; p < last && p < page_count; p++) {
            if (p < LOW_MEMORY_END / PAGE_SIZE || !page_used(p))
                continue;
            mark_free(p);
            free_pages++;
            total_pages++;
            reclaimed++;
        }
    }
    spin_unlock_irqrestore(&lock, flags);

    kfree(ranges);
    return reclaimed;
}

static bool skipped(const uint64_t *skip, size_t n, uint64_t p)
{
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (skip[mid] == p)
            return true;
        if (skip[mid] < p)
            lo = mid + 1;
        else
            hi = mid;
    }
    return false;
}

void pmm_wipe(const uint64_t *skip, size_t nskip)
{
    for (size_t r = 0; r < nram; r++) {
        for (uint64_t p = ram[r].base; p < ram[r].end; p += PAGE_SIZE) {
            if (skipped(skip, nskip, p))
                continue;
            void *v = phys_to_virt(p);
            uint64_t cnt = PAGE_SIZE / 8;
            __asm__ volatile ("rep stosq" : "+D"(v), "+c"(cnt) : "a"(0UL) : "memory");
        }
    }
}

size_t pmm_total_pages(void)
{
    return total_pages;
}

size_t pmm_free_count(void)
{
    return free_pages;
}

void pmm_ref(uint64_t phys)
{
    size_t i = phys / PAGE_SIZE;
    uint64_t flags = spin_lock_irqsave(&lock);
    if (i >= page_count || !page_used(i) || refs[i] == UINT16_MAX)
        panic("pmm: bad ref of %#lx", phys);
    refs[i]++;
    spin_unlock_irqrestore(&lock, flags);
}

unsigned pmm_refcount(uint64_t phys)
{
    size_t i = phys / PAGE_SIZE;
    return i < page_count ? refs[i] : 0;
}
