// lazily filled file mappings and the page cache behind them. a cached page
// is keyed by file and contents (size and mtime), so a rewritten file gets
// fresh pages and the stale ones age out
#include "mm/filemap.h"
#include "arch/x86_64/cpu.h"
#include "fs/vfs.h"
#include "lib/spinlock.h"
#include "lib/string.h"
#include "lib/printf.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define PROT_WRITE 2
#define PROT_EXEC  4
#define BUCKETS    4096

struct pcent {
    struct vnode *v;
    uint64_t ino, size, index, phys;
    int64_t mtime;
    struct pcent *next;
};

static struct pcent *cache[BUCKETS];
static size_t cached, limit;
static spinlock_t cache_lock = SPINLOCK_INIT;
static spinlock_t map_lock = SPINLOCK_INIT;

static unsigned bucket(struct vnode *v, uint64_t index)
{
    uint64_t h = (uint64_t)v * 0x9e3779b97f4a7c15ULL ^ index * 0xff51afd7ed558ccdULL;
    return (unsigned)(h >> 40) % BUCKETS;
}

static bool same(struct pcent *e, struct vnode *v, uint64_t index)
{
    return e->v == v && e->index == index && e->ino == v->ino && e->size == v->size &&
           e->mtime == v->mtime;
}

// lock held. drops pages only the cache still uses, until under the limit
static void shrink(void)
{
    for (unsigned b = 0; b < BUCKETS && cached > limit * 3 / 4; b++) {
        for (struct pcent **pp = &cache[b]; *pp;) {
            struct pcent *e = *pp;
            if (pmm_refcount(e->phys) == 1) {
                *pp = e->next;
                pmm_free(e->phys);
                kfree(e);
                cached--;
            } else {
                pp = &e->next;
            }
        }
    }
}

// the page of v at index, with a reference for the caller. 0 when out of memory
static uint64_t cache_get(struct vnode *v, uint64_t index)
{
    unsigned b = bucket(v, index);
    uint64_t irq = spin_lock_irqsave(&cache_lock);
    for (struct pcent *e = cache[b]; e; e = e->next) {
        if (same(e, v, index)) {
            pmm_ref(e->phys);
            spin_unlock_irqrestore(&cache_lock, irq);
            return e->phys;
        }
    }
    spin_unlock_irqrestore(&cache_lock, irq);

    uint64_t p = pmm_alloc_zeroed();
    struct pcent *n = kmalloc(sizeof *n);
    if (!p || !n) {
        if (p)
            pmm_free(p);
        kfree(n);
        return 0;
    }
    uint64_t off = index * PAGE_SIZE;
    if (off < v->size) {
        size_t want = v->size - off < PAGE_SIZE ? v->size - off : PAGE_SIZE;
        int64_t got = v->ops->read(v, phys_to_virt(p), want, off);
        if (got < 0) {
            pmm_free(p);
            kfree(n);
            return 0;
        }
    }
    *n = (struct pcent){ v, v->ino, v->size, index, p, v->mtime, NULL };

    irq = spin_lock_irqsave(&cache_lock);
    // someone may have read it meanwhile
    for (struct pcent *e = cache[b]; e; e = e->next) {
        if (same(e, v, index)) {
            pmm_ref(e->phys);
            spin_unlock_irqrestore(&cache_lock, irq);
            pmm_free(p);
            kfree(n);
            return e->phys;
        }
    }
    if (!limit)
        limit = pmm_total_pages() / 4;
    n->next = cache[b];
    cache[b] = n;
    cached++;
    pmm_ref(p);    // one for the cache, one for the caller
    if (cached > limit)
        shrink();
    spin_unlock_irqrestore(&cache_lock, irq);
    return p;
}

static uint64_t pte_flags(int prot)
{
    return (prot & PROT_WRITE ? PTE_WRITE : 0) | (prot & PROT_EXEC ? 0 : PTE_NX);
}

int filemap_add(struct address_space *as, uint64_t start, uint64_t len, struct vnode *v,
                uint64_t off, int prot, const char *path)
{
    struct fmap *m = kmalloc(sizeof *m);
    if (!m)
        return ZK_ENOMEM;
    vnode_get(v);
    *m = (struct fmap){ start, start + len, v, off, prot, "", NULL };
    const char *base = path ? strrchr(path, '/') : NULL;
    strlcpy(m->name, base ? base + 1 : path ? path : "?", sizeof m->name);
    uint64_t irq = spin_lock_irqsave(&map_lock);
    m->next = as->fmaps;
    as->fmaps = m;
    spin_unlock_irqrestore(&map_lock, irq);
    return 0;
}

// lock held. splits m at addr, the part from addr on comes after it
static bool split(struct fmap *m, uint64_t addr)
{
    if (addr <= m->start || addr >= m->end)
        return true;
    struct fmap *n = kmalloc(sizeof *n);
    if (!n)
        return false;
    vnode_get(m->v);
    *n = *m;
    n->start = addr;
    n->off = m->off + (addr - m->start);
    m->end = addr;
    m->next = n;
    return true;
}

void filemap_remove(struct address_space *as, uint64_t start, uint64_t len)
{
    uint64_t end = start + len;
    struct fmap *dead = NULL;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    for (struct fmap **pp = &as->fmaps; *pp;) {
        struct fmap *m = *pp;
        if (m->end <= start || m->start >= end) {
            pp = &m->next;
            continue;
        }
        split(m, start);
        split(m, end);
        if (m->start >= start && m->end <= end) {
            *pp = m->next;
            m->next = dead;
            dead = m;
        } else {
            pp = &m->next;
        }
    }
    spin_unlock_irqrestore(&map_lock, irq);
    while (dead) {
        struct fmap *n = dead->next;
        vnode_put(dead->v);
        kfree(dead);
        dead = n;
    }
}

void filemap_protect(struct address_space *as, uint64_t start, uint64_t len, int prot)
{
    uint64_t end = start + len;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    for (struct fmap *m = as->fmaps; m; m = m->next) {
        if (m->end <= start || m->start >= end)
            continue;
        split(m, start);
        split(m, end);
        if (m->start >= start && m->end <= end)
            m->prot = prot;
    }
    spin_unlock_irqrestore(&map_lock, irq);
}

bool filemap_describe(struct address_space *as, uint64_t addr, char *out, size_t size)
{
    bool found = false;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    for (struct fmap *m = as->fmaps; m && !found; m = m->next) {
        if (addr >= m->start && addr < m->end) {
            snprintf(out, size, "%s+%#lx", m->name, m->off + (addr - m->start));
            found = true;
        }
    }
    spin_unlock_irqrestore(&map_lock, irq);
    return found;
}

bool filemap_covers(struct address_space *as, uint64_t addr)
{
    bool found = false;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    for (struct fmap *m = as->fmaps; m && !found; m = m->next)
        found = addr >= m->start && addr < m->end;
    spin_unlock_irqrestore(&map_lock, irq);
    return found;
}

bool filemap_fault(struct address_space *as, uint64_t addr, bool write)
{
    uint64_t va = ALIGN_DOWN(addr, PAGE_SIZE);
    struct vnode *v = NULL;
    uint64_t off = 0;
    int prot = 0;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    for (struct fmap *m = as->fmaps; m; m = m->next) {
        if (va >= m->start && va < m->end) {
            v = m->v;
            vnode_get(v);
            off = m->off + (va - m->start);
            prot = m->prot;
            break;
        }
    }
    spin_unlock_irqrestore(&map_lock, irq);
    if (!v)
        return false;
    if (!prot || (write && !(prot & PROT_WRITE))) {
        vnode_put(v);
        return false;
    }

    // the read may sleep on the disk
    uint64_t fl = irq_save();
    sti();
    uint64_t p = cache_get(v, off / PAGE_SIZE);
    irq_restore(fl);
    vnode_put(v);
    if (!p)
        return false;

    uint64_t flags = pte_flags(prot);
    if (prot & PROT_WRITE) {
        if (write) {
            // our own copy straight away
            uint64_t c = pmm_alloc();
            if (!c) {
                pmm_free(p);
                return false;
            }
            memcpy(phys_to_virt(c), phys_to_virt(p), PAGE_SIZE);
            pmm_free(p);
            p = c;
        } else {
            flags = (flags & ~PTE_WRITE) | PTE_COW;
        }
    }
    // another thread got there first
    if (vmm_translate_user(as, va, NULL)) {
        pmm_free(p);
        return true;
    }
    if (!vmm_map_user(as, va, p, flags)) {
        pmm_free(p);
        return false;
    }
    return true;
}

bool filemap_clone(struct address_space *dst, struct address_space *src)
{
    bool ok = true;
    uint64_t irq = spin_lock_irqsave(&map_lock);
    struct fmap **tail = &dst->fmaps;
    for (struct fmap *m = src->fmaps; m; m = m->next) {
        struct fmap *n = kmalloc(sizeof *n);
        if (!n) {
            ok = false;
            break;
        }
        *n = *m;
        n->next = NULL;
        vnode_get(n->v);
        *tail = n;
        tail = &n->next;
    }
    spin_unlock_irqrestore(&map_lock, irq);
    return ok;
}

void filemap_destroy(struct address_space *as)
{
    uint64_t irq = spin_lock_irqsave(&map_lock);
    struct fmap *m = as->fmaps;
    as->fmaps = NULL;
    spin_unlock_irqrestore(&map_lock, irq);
    while (m) {
        struct fmap *n = m->next;
        vnode_put(m->v);
        kfree(m);
        m = n;
    }
}
