// first fit allocator with boundary tags. blocks coalesce on free and the
// heap grows through alloc_pages, which hands out contiguous pages
#include "zk.h"

#define PAGE_SIZE  4096
#define ALIGN      16
#define HDR        sizeof(struct blk)
#define MIN_BLOCK  32
#define GROW_MIN   (64 * 1024)
#define USED       1UL

struct blk {
    size_t size;    // whole block including header, low bit set when used
    size_t prev;    // size of the block before it, 0 for the first one
};

struct fnode {
    struct blk h;
    struct fnode *next, *prev_free;
};

static struct fnode *free_list;
static uint8_t *heap_end;    // the sentinel header lives just below this

static size_t bsize(struct blk *b) { return b->size & ~USED; }
static bool used(struct blk *b) { return b->size & USED; }
static struct blk *next_blk(struct blk *b) { return (struct blk *)((uint8_t *)b + bsize(b)); }
static struct blk *prev_blk(struct blk *b) { return (struct blk *)((uint8_t *)b - b->prev); }

static void list_add(struct fnode *f)
{
    f->prev_free = NULL;
    f->next = free_list;
    if (free_list)
        free_list->prev_free = f;
    free_list = f;
}

static void list_del(struct fnode *f)
{
    if (f->prev_free)
        f->prev_free->next = f->next;
    else
        free_list = f->next;
    if (f->next)
        f->next->prev_free = f->prev_free;
}

static void set_size(struct blk *b, size_t size, bool in_use)
{
    b->size = size | (in_use ? USED : 0);
    next_blk(b)->prev = size;
}

// merges b with free neighbours, puts the result on the free list
static void release(struct blk *b)
{
    size_t size = bsize(b);
    struct blk *n = next_blk(b);
    if (!used(n)) {
        list_del((struct fnode *)n);
        size += bsize(n);
    }
    if (b->prev && !used(prev_blk(b))) {
        struct blk *p = prev_blk(b);
        list_del((struct fnode *)p);
        size += bsize(p);
        b = p;
    }
    set_size(b, size, false);
    list_add((struct fnode *)b);
}

static bool grow(size_t need)
{
    size_t bytes = need + HDR > GROW_MIN ? need + HDR : GROW_MIN;
    size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *p = alloc_pages(pages);
    if (!p)
        return false;
    uint8_t *end = p + pages * PAGE_SIZE;

    struct blk *b;
    if (heap_end && p == heap_end) {
        // the old sentinel becomes the start of the new space
        b = (struct blk *)(heap_end - HDR);
        b->size = (size_t)(end - HDR - (uint8_t *)b) | USED;
    } else {
        b = (struct blk *)p;
        b->prev = 0;
        b->size = (size_t)(pages * PAGE_SIZE - HDR) | USED;
    }
    struct blk *sentinel = (struct blk *)(end - HDR);
    sentinel->size = USED;    // size 0, never merged
    sentinel->prev = bsize(b);
    heap_end = end;
    release(b);
    return true;
}

static size_t block_for(size_t n)
{
    if (n > (size_t)1 << 40)
        return 0;
    size_t size = (n + HDR + ALIGN - 1) & ~(size_t)(ALIGN - 1);
    return size < MIN_BLOCK ? MIN_BLOCK : size;
}

// cuts b down to size, freeing the tail if it's big enough
static void split(struct blk *b, size_t size)
{
    size_t total = bsize(b);
    if (total - size < MIN_BLOCK) {
        set_size(b, total, true);
        return;
    }
    set_size(b, size, true);
    struct blk *rest = next_blk(b);
    rest->size = USED;
    set_size(rest, total - size, true);
    release(rest);
}

void *malloc(size_t n)
{
    size_t size = block_for(n);
    if (!size)
        return NULL;
    for (int tries = 0; tries < 2; tries++) {
        for (struct fnode *f = free_list; f; f = f->next) {
            if (bsize(&f->h) >= size) {
                list_del(f);
                split(&f->h, size);
                return (uint8_t *)f + HDR;
            }
        }
        if (!grow(size))
            return NULL;
    }
    return NULL;
}

void free(void *ptr)
{
    if (!ptr)
        return;
    struct blk *b = (struct blk *)((uint8_t *)ptr - HDR);
    if (!used(b)) {
        dprintf(2, "free: double free of %p\n", ptr);
        exit(134);
    }
    release(b);
}

void *calloc(size_t count, size_t size)
{
    if (size && count > (size_t)-1 / size)
        return NULL;
    void *p = malloc(count * size);
    if (p)
        memset(p, 0, count * size);
    return p;
}

void *realloc(void *ptr, size_t n)
{
    if (!ptr)
        return malloc(n);
    if (n == 0) {
        free(ptr);
        return NULL;
    }
    size_t size = block_for(n);
    if (!size)
        return NULL;
    struct blk *b = (struct blk *)((uint8_t *)ptr - HDR);
    size_t have = bsize(b);
    if (have >= size) {
        split(b, size);
        return ptr;
    }
    struct blk *nb = next_blk(b);
    if (!used(nb) && have + bsize(nb) >= size) {
        list_del((struct fnode *)nb);
        set_size(b, have + bsize(nb), true);
        split(b, size);
        return ptr;
    }
    void *p = malloc(n);
    if (p) {
        memcpy(p, ptr, have - HDR);
        free(ptr);
    }
    return p;
}
