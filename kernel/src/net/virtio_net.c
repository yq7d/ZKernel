// virtio network card, legacy pci interface. receive buffers are polled by
// the net thread, sends wait for the device to take the frame
#include "net/net.h"
#include "drivers/pci.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"

#define VIRTIO_VENDOR     0x1AF4
#define VIRTIO_NET_LEGACY 0x1000

#define REG_DEV_FEATURES   0x00
#define REG_GUEST_FEATURES 0x04
#define REG_QUEUE_ADDR     0x08
#define REG_QUEUE_SIZE     0x0C
#define REG_QUEUE_SELECT   0x0E
#define REG_QUEUE_NOTIFY   0x10
#define REG_STATUS         0x12
#define REG_ISR            0x13
#define REG_MAC            0x14

#define F_MAC (1U << 5)

#define DESC_WRITE 2

#define RX_BUFS  256
#define TX_BUFS  16
#define BUF_SIZE 2048
#define HDR_LEN  10

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
    uint32_t id, len;
} __attribute__((packed));

struct used {
    uint16_t flags;
    uint16_t idx;
    struct used_elem ring[];
} __attribute__((packed));

struct vq {
    uint16_t size;
    struct desc *desc;
    volatile struct avail *avail;
    volatile struct used *used;
    uint16_t last_used;
    uint8_t *bufs;
    uint64_t bufs_phys;
};

static int virtio_net_send(const void *frame, size_t len);
static int virtio_net_poll(void);
static bool virtio_net_pending(void);
static const struct nic_ops ops = { "virtio-net", virtio_net_send, virtio_net_poll,
                                    virtio_net_pending };

static uint16_t io;
static struct vq rx, tx;
static bool present;
static uint16_t tx_next;

static bool setup_queue(uint16_t index, struct vq *q, int nbufs)
{
    outw(io + REG_QUEUE_SELECT, index);
    q->size = inw(io + REG_QUEUE_SIZE);
    if (q->size < nbufs)
        return false;
    size_t ring1 = ALIGN_UP(16UL * q->size + 6 + 2UL * q->size, PAGE_SIZE);
    size_t ring2 = ALIGN_UP(6 + 8UL * q->size, PAGE_SIZE);
    size_t pages = (ring1 + ring2) / PAGE_SIZE;
    uint64_t mem = pmm_alloc_pages(pages);
    size_t buf_pages = (size_t)nbufs * BUF_SIZE / PAGE_SIZE;
    uint64_t bufs = pmm_alloc_pages(buf_pages);
    if (!mem || !bufs)
        return false;
    memset(phys_to_virt(mem), 0, pages * PAGE_SIZE);
    memset(phys_to_virt(bufs), 0, buf_pages * PAGE_SIZE);
    q->desc = phys_to_virt(mem);
    q->avail = (void *)((uint8_t *)q->desc + 16UL * q->size);
    q->used = (void *)((uint8_t *)q->desc + ring1);
    q->bufs = phys_to_virt(bufs);
    q->bufs_phys = bufs;
    outl(io + REG_QUEUE_ADDR, (uint32_t)(mem / PAGE_SIZE));
    return true;
}

static void post_rx(uint16_t id)
{
    rx.desc[id] = (struct desc){ rx.bufs_phys + (uint64_t)id * BUF_SIZE, BUF_SIZE, DESC_WRITE, 0 };
    rx.avail->ring[rx.avail->idx % rx.size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rx.avail->idx++;
}

bool virtio_net_init(void)
{
    const struct pci_dev *d = pci_find(VIRTIO_VENDOR, VIRTIO_NET_LEGACY, NULL);
    if (!d)
        return false;
    bool is_io;
    uint64_t bar = pci_bar(d, 0, &is_io);
    if (!is_io || !bar)
        return false;
    pci_enable(d);
    io = (uint16_t)bar;

    outb(io + REG_STATUS, 0);
    outb(io + REG_STATUS, 1);        // acknowledge
    outb(io + REG_STATUS, 1 | 2);    // driver
    uint32_t features = inl(io + REG_DEV_FEATURES);
    outl(io + REG_GUEST_FEATURES, features & F_MAC);
    if (!setup_queue(0, &rx, RX_BUFS) || !setup_queue(1, &tx, TX_BUFS)) {
        outb(io + REG_STATUS, 128);
        return false;
    }
    rx.avail->flags = 1;    // no interrupts, the net thread polls
    tx.avail->flags = 1;
    for (uint16_t i = 0; i < RX_BUFS; i++)
        post_rx(i);
    outb(io + REG_STATUS, 1 | 2 | 4);
    outw(io + REG_QUEUE_NOTIFY, 0);

    for (int i = 0; i < 6; i++)
        netif.mac[i] = (features & F_MAC) ? inb(io + REG_MAC + i) : (uint8_t)(0x52 + i);
    present = true;
    nic = &ops;
    return true;
}

static int virtio_net_send(const void *frame, size_t len)
{
    if (!present)
        return ZK_ENODEV;
    if (len > BUF_SIZE - HDR_LEN)
        return ZK_EINVAL;
    uint64_t flags = irq_save();
    uint16_t id = tx_next++ % TX_BUFS;
    uint8_t *buf = tx.bufs + (size_t)id * BUF_SIZE;
    memset(buf, 0, HDR_LEN);
    memcpy(buf + HDR_LEN, frame, len);
    tx.desc[id] = (struct desc){ tx.bufs_phys + (uint64_t)id * BUF_SIZE, (uint32_t)(len + HDR_LEN), 0, 0 };
    tx.avail->ring[tx.avail->idx % tx.size] = id;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    tx.avail->idx++;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    outw(io + REG_QUEUE_NOTIFY, 1);
    // the buffer is reused soon, so wait for the device to be done with it
    for (int spin = 0; spin < 1000000 && tx.used->idx == tx.last_used; spin++)
        __asm__ volatile ("pause");
    tx.last_used = tx.used->idx;
    irq_restore(flags);
    return 0;
}

static bool virtio_net_pending(void)
{
    return present && rx.used->idx != rx.last_used;
}

static int virtio_net_poll(void)
{
    if (!present)
        return 0;
    int n = 0;
    while (rx.used->idx != rx.last_used) {
        struct used_elem e = rx.used->ring[rx.last_used % rx.size];
        rx.last_used++;
        if (e.id < RX_BUFS && e.len > HDR_LEN)
            net_input(rx.bufs + (size_t)e.id * BUF_SIZE + HDR_LEN, e.len - HDR_LEN);
        post_rx((uint16_t)e.id);
        n++;
    }
    if (n) {
        outw(io + REG_QUEUE_NOTIFY, 0);
        inb(io + REG_ISR);
    }
    return n;
}
