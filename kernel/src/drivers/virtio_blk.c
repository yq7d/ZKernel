// virtio block devices through the legacy (0.9.5) pci interface. one request
// in flight at a time, completion is polled
#include "drivers/virtio_blk.h"
#include "drivers/blk.h"
#include "drivers/pci.h"
#include "arch/x86_64/io.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"

#define VIRTIO_VENDOR     0x1AF4
#define VIRTIO_BLK_LEGACY 0x1001
#define VIRTIO_BLK_MODERN 0x1042

#define REG_DEV_FEATURES   0x00
#define REG_GUEST_FEATURES 0x04
#define REG_QUEUE_ADDR     0x08
#define REG_QUEUE_SIZE     0x0C
#define REG_QUEUE_SELECT   0x0E
#define REG_QUEUE_NOTIFY   0x10
#define REG_STATUS         0x12
#define REG_ISR            0x13
#define REG_CONFIG         0x14

#define STATUS_ACK       1
#define STATUS_DRIVER    2
#define STATUS_DRIVER_OK 4
#define STATUS_FAILED    128

#define F_RO (1U << 5)

#define DESC_NEXT  1
#define DESC_WRITE 2

#define REQ_IN  0
#define REQ_OUT 1

#define BOUNCE_PAGES   16
#define MAX_SECTORS    (BOUNCE_PAGES * PAGE_SIZE / SECTOR_SIZE)

struct desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
} __attribute__((packed));

struct used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct used {
    uint16_t flags;
    uint16_t idx;
    struct used_elem ring[];
} __attribute__((packed));

struct req_hdr {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
} __attribute__((packed));

struct vblk {
    struct blkdev blk;
    uint16_t io;
    uint16_t qsize;
    bool read_only;
    struct desc *desc;
    volatile struct avail *avail;
    volatile struct used *used;
    uint16_t last_used;
    struct req_hdr *hdr;
    volatile uint8_t *status;
    uint64_t hdr_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
};

static int ndisks;

static int request(struct vblk *v, uint32_t type, uint64_t lba, uint32_t count)
{
    v->hdr->type = type;
    v->hdr->reserved = 0;
    v->hdr->sector = lba;
    *v->status = 0xFF;

    v->desc[0] = (struct desc){ v->hdr_phys, sizeof(struct req_hdr), DESC_NEXT, 1 };
    v->desc[1] = (struct desc){ v->bounce_phys, count * SECTOR_SIZE,
                                (uint16_t)(DESC_NEXT | (type == REQ_IN ? DESC_WRITE : 0)), 2 };
    v->desc[2] = (struct desc){ v->hdr_phys + 512, 1, DESC_WRITE, 0 };

    v->avail->ring[v->avail->idx % v->qsize] = 0;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    v->avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    outw(v->io + REG_QUEUE_NOTIFY, 0);

    while (v->used->idx == v->last_used)
        __asm__ volatile ("pause");
    v->last_used++;
    inb(v->io + REG_ISR);    // ack, in case interrupts are on
    return *v->status == 0 ? 0 : ZK_EIO;
}

static int vread(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    struct vblk *v = d->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        int err = request(v, REQ_IN, lba, n);
        if (err)
            return err;
        memcpy(out, v->bounce, n * SECTOR_SIZE);
        out += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int vwrite(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct vblk *v = d->priv;
    if (v->read_only)
        return ZK_EROFS;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        memcpy(v->bounce, in, n * SECTOR_SIZE);
        int err = request(v, REQ_OUT, lba, n);
        if (err)
            return err;
        in += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static bool setup(const struct pci_dev *pd)
{
    bool is_io;
    uint64_t bar = pci_bar(pd, 0, &is_io);
    if (!is_io || !bar)
        return false;
    pci_enable(pd);

    struct vblk *v = kzalloc(sizeof(*v));
    if (!v)
        return false;
    v->io = (uint16_t)bar;

    outb(v->io + REG_STATUS, 0);    // reset
    outb(v->io + REG_STATUS, STATUS_ACK);
    outb(v->io + REG_STATUS, STATUS_ACK | STATUS_DRIVER);
    uint32_t features = inl(v->io + REG_DEV_FEATURES);
    v->read_only = features & F_RO;
    outl(v->io + REG_GUEST_FEATURES, features & F_RO);

    outw(v->io + REG_QUEUE_SELECT, 0);
    v->qsize = inw(v->io + REG_QUEUE_SIZE);
    if (v->qsize < 3)
        goto fail;

    size_t ring1 = ALIGN_UP(16UL * v->qsize + 6 + 2UL * v->qsize, PAGE_SIZE);
    size_t ring2 = ALIGN_UP(6 + 8UL * v->qsize, PAGE_SIZE);
    size_t pages = (ring1 + ring2) / PAGE_SIZE;
    uint64_t q = pmm_alloc_pages(pages);
    uint64_t hdr = pmm_alloc();
    uint64_t bounce = pmm_alloc_pages(BOUNCE_PAGES);
    if (!q || !hdr || !bounce)
        goto fail;    // a few pages leak on this path, it only happens at boot
    memset(phys_to_virt(q), 0, pages * PAGE_SIZE);

    v->desc = phys_to_virt(q);
    v->avail = (void *)((uint8_t *)v->desc + 16UL * v->qsize);
    v->used = (void *)((uint8_t *)v->desc + ring1);
    v->avail->flags = 1;    // no interrupts, we poll
    v->hdr_phys = hdr;
    v->hdr = phys_to_virt(hdr);
    v->status = (uint8_t *)v->hdr + 512;
    v->bounce_phys = bounce;
    v->bounce = phys_to_virt(bounce);
    outl(v->io + REG_QUEUE_ADDR, (uint32_t)(q / PAGE_SIZE));

    outb(v->io + REG_STATUS, STATUS_ACK | STATUS_DRIVER | STATUS_DRIVER_OK);

    uint64_t cap = inl(v->io + REG_CONFIG) | ((uint64_t)inl(v->io + REG_CONFIG + 4) << 32);
    snprintf(v->blk.name, sizeof v->blk.name, "vd%c", 'a' + ndisks++);
    v->blk.sectors = cap;
    v->blk.read = vread;
    v->blk.write = vwrite;
    v->blk.priv = v;
    blk_register(&v->blk);
    pr_ok("virtio-blk: /dev/%s, %lu MiB%s\n", v->blk.name, cap * SECTOR_SIZE >> 20,
          v->read_only ? ", read only" : "");
    return true;

fail:
    outb(v->io + REG_STATUS, STATUS_FAILED);
    kfree(v);
    return false;
}

void virtio_blk_init(void)
{
    for (const struct pci_dev *d = NULL; (d = pci_find(VIRTIO_VENDOR, VIRTIO_BLK_LEGACY, d)); )
        if (!setup(d))
            pr_warn("virtio-blk: setup failed at %02x:%02x.%x\n", d->bus, d->dev, d->fn);
    for (const struct pci_dev *d = NULL; (d = pci_find(VIRTIO_VENDOR, VIRTIO_BLK_MODERN, d)); )
        pr_warn("virtio-blk: modern-only device at %02x:%02x.%x not supported\n",
                d->bus, d->dev, d->fn);
}
