// memory files. a sparse array of physical pages, grown as it's written or
// mapped. the pages are refcounted, a mapping holds its own reference
#include "fs/shmem.h"
#include "fs/vfs.h"
#include "arch/x86_64/cpu.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include <zk/syscall.h>

#define MAX_PAGES (1UL << 20)    // 4 GiB

struct shmem {
    struct vnode v;
    uint64_t *pages;
    size_t cap;
};

static const struct vnode_ops shmem_ops;

bool shmem_is(struct vnode *v)
{
    return v && v->ops == &shmem_ops;
}

static bool grow(struct shmem *s, size_t need)
{
    if (need <= s->cap)
        return true;
    if (need > MAX_PAGES)
        return false;
    size_t cap = s->cap ? s->cap : 16;
    while (cap < need)
        cap *= 2;
    uint64_t *n = kzalloc(cap * sizeof(*n));
    if (!n)
        return false;
    if (s->pages)
        memcpy(n, s->pages, s->cap * sizeof(*n));
    kfree(s->pages);
    s->pages = n;
    s->cap = cap;
    return true;
}

uint64_t shmem_page(struct vnode *v, uint64_t index)
{
    struct shmem *s = (struct shmem *)v;
    uint64_t irq = irq_save();
    uint64_t p = 0;
    if (grow(s, index + 1)) {
        p = s->pages[index];
        if (!p) {
            p = pmm_alloc_zeroed();
            s->pages[index] = p;
        }
    }
    irq_restore(irq);
    return p;
}

static int64_t sh_read(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct shmem *s = (struct shmem *)v;
    if (off >= v->size)
        return 0;
    if (len > v->size - off)
        len = v->size - off;
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? PAGE_SIZE - in : len - done;
        uint64_t p = idx < s->cap ? s->pages[idx] : 0;
        if (p)
            memcpy((uint8_t *)buf + done, (uint8_t *)phys_to_virt(p) + in, n);
        else
            memset((uint8_t *)buf + done, 0, n);
        done += n;
    }
    return (int64_t)done;
}

static int64_t sh_write(struct vnode *v, const void *buf, size_t len, uint64_t off)
{
    size_t done = 0;
    while (done < len) {
        uint64_t pos = off + done;
        uint64_t idx = pos / PAGE_SIZE, in = pos % PAGE_SIZE;
        size_t n = PAGE_SIZE - in < len - done ? PAGE_SIZE - in : len - done;
        uint64_t p = shmem_page(v, idx);
        if (!p)
            return done ? (int64_t)done : ZK_ENOSPC;
        memcpy((uint8_t *)phys_to_virt(p) + in, (const uint8_t *)buf + done, n);
        done += n;
    }
    if (off + done > v->size)
        v->size = off + done;
    return (int64_t)done;
}

static int sh_truncate(struct vnode *v, uint64_t size)
{
    struct shmem *s = (struct shmem *)v;
    if (size / PAGE_SIZE > MAX_PAGES)
        return ZK_EFBIG;
    uint64_t irq = irq_save();
    // pages past the end go, mappings keep theirs alive
    uint64_t keep = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    for (uint64_t i = keep; i < s->cap; i++) {
        if (s->pages[i]) {
            pmm_free(s->pages[i]);
            s->pages[i] = 0;
        }
    }
    // the tail of a partial last page reads as zeros again
    if (size % PAGE_SIZE && keep - 1 < s->cap && s->pages[keep - 1] && size < v->size) {
        uint64_t in = size % PAGE_SIZE;
        memset((uint8_t *)phys_to_virt(s->pages[keep - 1]) + in, 0, PAGE_SIZE - in);
    }
    v->size = size;
    irq_restore(irq);
    return 0;
}

static void sh_release(struct vnode *v)
{
    struct shmem *s = (struct shmem *)v;
    for (size_t i = 0; i < s->cap; i++)
        if (s->pages[i])
            pmm_free(s->pages[i]);
    kfree(s->pages);
    kfree(s);
}

static const struct vnode_ops shmem_ops = {
    .read = sh_read, .write = sh_write, .truncate = sh_truncate, .release = sh_release,
};

struct vnode *shmem_new(uint64_t size)
{
    struct shmem *s = kzalloc(sizeof(*s));
    if (!s)
        return NULL;
    s->v = (struct vnode){ .type = VNODE_FILE, .refs = 1, .ino = vfs_next_ino(),
                           .mode = 0600, .ops = &shmem_ops, .size = size };
    return &s->v;
}
