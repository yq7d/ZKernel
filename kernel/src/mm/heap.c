// first fit allocator over one contiguous virtual region that grows on demand.
// blocks tile the region exactly, each knows its size and the block before it,
// so neighbours can be merged on free.
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"
#include "lib/spinlock.h"
#include "lib/string.h"
#include "panic.h"
#include <stdbool.h>
#include <stdint.h>

#define BLOCK_MAGIC  0x4b484541u
#define ALIGN        16
#define MIN_SPLIT    (sizeof(struct block) + ALIGN)
#define INITIAL_SIZE (64 * 1024)
#define POISON       0xDE

struct block {
    uint32_t magic;
    uint32_t free;
    size_t size;            // whole block including this header
    struct block *prev;
    uint64_t pad;
};

_Static_assert(sizeof(struct block) % ALIGN == 0, "block header breaks alignment");

static uint64_t heap_end;
static struct block *first, *last;
static spinlock_t lock = SPINLOCK_INIT;

static struct block *next_block(struct block *b)
{
    uint64_t n = (uint64_t)b + b->size;
    return n < heap_end ? (struct block *)n : NULL;
}

static void check(struct block *b, const char *what)
{
    if (b->magic != BLOCK_MAGIC)
        panic("heap: %s: corrupted block at %p", what, (void *)b);
}

static size_t grow(size_t bytes)
{
    bytes = ALIGN_UP(bytes, PAGE_SIZE);
    if (heap_end + bytes > HEAP_LIMIT)
        return 0;
    for (uint64_t v = heap_end; v < heap_end + bytes; v += PAGE_SIZE) {
        uint64_t p = pmm_alloc();
        if (!p || !vmm_map(v, p, PTE_WRITE | PTE_NX)) {
            if (p)
                pmm_free(p);
            bytes = v - heap_end;
            break;
        }
    }
    heap_end += bytes;
    return bytes;
}

void heap_init(void)
{
    heap_end = HEAP_BASE;
    size_t got = grow(INITIAL_SIZE);
    if (!got)
        panic("heap: can't map initial heap");

    first = last = (struct block *)HEAP_BASE;
    *first = (struct block){ .magic = BLOCK_MAGIC, .free = 1, .size = got };
}

static void split(struct block *b, size_t need)
{
    if (b->size - need < MIN_SPLIT)
        return;

    struct block *n = (struct block *)((uint64_t)b + need);
    *n = (struct block){ .magic = BLOCK_MAGIC, .free = 1,
                         .size = b->size - need, .prev = b };
    struct block *after = next_block(n);
    if (after)
        after->prev = n;
    else
        last = n;
    b->size = need;
}

static void *kmalloc_locked(size_t need)
{
    for (struct block *b = first; b; b = next_block(b)) {
        check(b, "kmalloc");
        if (b->free && b->size >= need) {
            split(b, need);
            b->free = 0;
            return b + 1;
        }
    }

    // nothing fits, extend the heap and use the tail
    size_t want = last->free ? need - last->size : need;
    uint64_t old_end = heap_end;
    size_t got = grow(want);

    // whatever got mapped has to belong to a block, even on failure
    if (got) {
        if (last->free) {
            last->size += got;
        } else {
            struct block *n = (struct block *)old_end;
            *n = (struct block){ .magic = BLOCK_MAGIC, .free = 1, .size = got, .prev = last };
            last = n;
        }
    }
    if (got < want)
        return NULL;

    struct block *b = last;
    split(b, need);
    b->free = 0;
    return b + 1;
}

void *kmalloc(size_t size)
{
    if (size == 0 || size > HEAP_LIMIT - HEAP_BASE)
        return NULL;
    size_t need = ALIGN_UP(size, ALIGN) + sizeof(struct block);

    uint64_t flags = spin_lock_irqsave(&lock);
    void *p = kmalloc_locked(need);
    spin_unlock_irqrestore(&lock, flags);
    return p;
}

void *kzalloc(size_t size)
{
    void *p = kmalloc(size);
    if (p)
        memset(p, 0, size);
    return p;
}

void *kcalloc(size_t count, size_t size)
{
    if (size && count > SIZE_MAX / size)
        return NULL;
    return kzalloc(count * size);
}

// absorb b's successor into b
static void merge_next(struct block *b)
{
    struct block *n = next_block(b);
    b->size += n->size;
    n->magic = 0;
    struct block *after = next_block(b);
    if (after)
        after->prev = b;
    else
        last = b;
}

void kfree(void *ptr)
{
    if (!ptr)
        return;

    struct block *b = (struct block *)ptr - 1;
    uint64_t flags = spin_lock_irqsave(&lock);
    if ((uint64_t)b < HEAP_BASE || (uint64_t)b >= heap_end)
        panic("heap: kfree of non-heap pointer %p", ptr);
    check(b, "kfree");
    if (b->free)
        panic("heap: double free of %p", ptr);

    memset(ptr, POISON, b->size - sizeof(struct block));
    b->free = 1;

    struct block *n = next_block(b);
    if (n && n->free)
        merge_next(b);
    if (b->prev && b->prev->free)
        merge_next(b->prev);
    spin_unlock_irqrestore(&lock, flags);
}

void *krealloc(void *ptr, size_t size)
{
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    struct block *b = (struct block *)ptr - 1;
    uint64_t flags = spin_lock_irqsave(&lock);
    check(b, "krealloc");
    size_t have = b->size - sizeof(struct block);
    spin_unlock_irqrestore(&lock, flags);
    if (size <= have)
        return ptr;

    void *n = kmalloc(size);
    if (!n)
        return NULL;
    memcpy(n, ptr, have);
    kfree(ptr);
    return n;
}

void heap_get_stats(struct heap_stats *s)
{
    uint64_t flags = spin_lock_irqsave(&lock);
    *s = (struct heap_stats){ .mapped = heap_end - HEAP_BASE };
    for (struct block *b = first; b; b = next_block(b)) {
        s->blocks++;
        if (b->free)
            s->free_blocks++;
        else
            s->used += b->size;
    }
    spin_unlock_irqrestore(&lock, flags);
}
