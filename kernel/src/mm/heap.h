#pragma once
#include <stddef.h>

void heap_init(void);

// 16 byte aligned
void *kmalloc(size_t size);
void *kzalloc(size_t size);
void *kcalloc(size_t count, size_t size);
void *krealloc(void *ptr, size_t size);
void kfree(void *ptr);

struct heap_stats {
    size_t mapped;       // bytes of virtual memory backing the heap
    size_t used;         // bytes in allocated blocks, headers included
    size_t blocks;
    size_t free_blocks;
};

void heap_get_stats(struct heap_stats *s);
