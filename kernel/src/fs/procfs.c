// /proc, files generated when read
#include "fs/vfs.h"
#include "drivers/blk.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "kprintf.h"
#include "drivers/rtc.h"
#include "cmdline.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "proc/proc.h"
#include <zk/version.h>

#define GEN_MAX (72 * 1024)

struct gen {
    char *buf;
    size_t len, size;
};

__attribute__((format(printf, 2, 3)))
static void out(struct gen *g, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g->buf + g->len, g->size - g->len, fmt, ap);
    va_end(ap);
    if (n > 0)
        g->len += (size_t)n < g->size - g->len ? (size_t)n : g->size - g->len - 1;
}

static void gen_version(struct gen *g)
{
    out(g, "ZKernel version " ZK_VERSION " (gcc) " __DATE__ " " __TIME__ "\n");
}

static void gen_uptime(struct gen *g)
{
    uint64_t ms = timer_uptime_ms();
    out(g, "%lu.%02lu\n", ms / 1000, (ms % 1000) / 10);
}

static void gen_meminfo(struct gen *g)
{
    struct heap_stats hs;
    heap_get_stats(&hs);
    out(g, "MemTotal:  %8zu kB\n", pmm_total_pages() * PAGE_SIZE >> 10);
    out(g, "MemFree:   %8zu kB\n", pmm_free_count() * PAGE_SIZE >> 10);
    out(g, "KernelHeap:%8zu kB\n", hs.mapped >> 10);
    out(g, "Ramfs:     %8zu kB\n", ramfs_bytes_used() >> 10);
}

static void mount_line(struct mount *m, void *ctx)
{
    out(ctx, "%s %s %s rw 0 0\n", m->source, m->path, m->type->name);
}

static void gen_mounts(struct gen *g)
{
    vfs_each_mount(mount_line, g);
}

static void gen_cmdline(struct gen *g)
{
    out(g, "%s\n", cmdline_raw());
}

static void gen_cpuinfo(struct gen *g)
{
    uint32_t a, b, c, d;
    char vendor[13];
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0));
    memcpy(vendor, &b, 4);
    memcpy(vendor + 4, &d, 4);
    memcpy(vendor + 8, &c, 4);
    vendor[12] = '\0';

    char brand[49] = "unknown";
    __asm__ volatile ("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000));
    if (a >= 0x80000004) {
        uint32_t *w = (uint32_t *)brand;
        for (uint32_t leaf = 0; leaf < 3; leaf++)
            __asm__ volatile ("cpuid" : "=a"(w[leaf * 4]), "=b"(w[leaf * 4 + 1]),
                              "=c"(w[leaf * 4 + 2]), "=d"(w[leaf * 4 + 3])
                              : "a"(0x80000002 + leaf));
        brand[48] = '\0';
    }
    const char *p = brand;
    while (*p == ' ')
        p++;
    out(g, "vendor_id  : %s\nmodel name : %s\n", vendor, p);
}

static void gen_kmsg(struct gen *g)
{
    g->len = klog_read(g->buf, g->size);
}

static void gen_pci(struct gen *g)
{
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        out(g, "%02x:%02x.%x %04x:%04x %s\n", d->bus, d->dev, d->fn, d->vendor, d->device,
            pci_class_name(d->class, d->subclass));
    }
}

static void gen_disks(struct gen *g)
{
    struct blk_stats s;
    blk_get_stats(&s);
    for (int i = 0; i < blk_count(); i++) {
        struct blkdev *d = blk_get(i);
        out(g, "%s %lu %s %s\n", d->name, d->sectors * SECTOR_SIZE, d->is_part ? "part" : "disk",
            d->model[0] ? d->model : "-");
    }
    out(g, "# cache hits %lu misses %lu writes %lu\n", s.hits, s.misses, s.writes);
}

static void gen_time(struct gen *g)
{
    out(g, "%ld\n", rtc_now());
}

static const struct {
    const char *name;
    void (*fn)(struct gen *g);
} files[] = {
    { "cmdline", gen_cmdline },
    { "cpuinfo", gen_cpuinfo },
    { "disks",   gen_disks },
    { "kmsg",    gen_kmsg },
    { "meminfo", gen_meminfo },
    { "mounts",  gen_mounts },
    { "pci",     gen_pci },
    { "time",    gen_time },
    { "uptime",  gen_uptime },
    { "version", gen_version },
};

#define NFILES (sizeof files / sizeof files[0])

static int64_t file_rd(struct vnode *v, void *buf, size_t len, uint64_t off)
{
    struct gen g = { kmalloc(GEN_MAX), 0, GEN_MAX };
    if (!g.buf)
        return ZK_ENOMEM;
    g.buf[0] = '\0';
    files[(size_t)v->priv].fn(&g);
    int64_t n = 0;
    if (off < g.len) {
        n = (int64_t)(g.len - off < len ? g.len - off : len);
        memcpy(buf, g.buf + off, (size_t)n);
    }
    kfree(g.buf);
    return n;
}

static const struct vnode_ops file_ops = { .read = file_rd };
static struct vnode nodes[NFILES];

static int dir_lookup(struct vnode *dir, const char *name, struct vnode **out_v)
{
    (void)dir;
    for (size_t i = 0; i < NFILES; i++) {
        if (strcmp(files[i].name, name) == 0) {
            vnode_get(&nodes[i]);
            *out_v = &nodes[i];
            return 0;
        }
    }
    return ZK_ENOENT;
}

static int dir_readdir(struct vnode *dir, size_t index, struct zk_dirent *d)
{
    (void)dir;
    if (index >= NFILES)
        return 0;
    d->type = ZK_TYPE_FILE;
    strlcpy(d->name, files[index].name, sizeof d->name);
    return 1;
}

static int dir_create(struct vnode *dir, const char *name, enum vnode_type type, struct vnode **out_v)
{
    (void)dir; (void)name; (void)type; (void)out_v;
    return ZK_EPERM;
}

static int dir_unlink(struct vnode *dir, const char *name)
{
    (void)dir; (void)name;
    return ZK_EPERM;
}

static const struct vnode_ops dir_ops = {
    .lookup = dir_lookup,
    .readdir = dir_readdir,
    .create = dir_create,
    .unlink = dir_unlink,
};

static struct vnode root = { .type = VNODE_DIR, .refs = 1, .ops = &dir_ops, .size = NFILES,
                             .mode = 0555 };

static int procfs_mount(struct mount *m, const char *source, const char *opts)
{
    (void)source; (void)opts;
    if (!root.ino) {
        root.ino = vfs_next_ino();
        for (size_t i = 0; i < NFILES; i++)
            nodes[i] = (struct vnode){ .type = VNODE_FILE, .refs = 1, .ino = vfs_next_ino(),
                                       .mode = 0444, .ops = &file_ops, .priv = (void *)i };
    }
    vnode_get(&root);
    m->root = &root;
    return 0;
}

const struct fs_type procfs_type = { "proc", procfs_mount };
