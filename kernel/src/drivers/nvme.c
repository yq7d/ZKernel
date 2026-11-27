// nvme ssds. one admin queue pair and one io queue pair per controller,
// completions are polled. namespaces show up as /dev/nvme0n1 and so on.
// only 512 byte lba formats, which is what nearly every drive ships with
#include "drivers/nvme.h"
#include "drivers/blk.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "kprintf.h"
#include "lib/printf.h"
#include "lib/string.h"
#include "mm/heap.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define REG_CAP  0x00
#define REG_CC   0x14
#define REG_CSTS 0x1C
#define REG_AQA  0x24
#define REG_ASQ  0x28
#define REG_ACQ  0x30
#define REG_DB   0x1000

#define QSIZE        64
#define BOUNCE_PAGES 16
#define MAX_SECTORS  (BOUNCE_PAGES * PAGE_SIZE / SECTOR_SIZE)
#define TIMEOUT_MS   5000

struct sqe {
    uint32_t cdw0, nsid, cdw2, cdw3;
    uint64_t mptr, prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct cqe {
    uint32_t result, reserved;
    uint16_t sq_head, sq_id;
    uint16_t cid, status;
};

struct queue {
    struct sqe *sq;
    volatile struct cqe *cq;
    uint16_t sq_tail, cq_head, phase, id;
};

struct ctrl {
    volatile uint8_t *regs;
    uint32_t stride;
    struct queue admin, io;
    uint8_t *bounce;
    uint64_t bounce_phys;
    uint64_t *prp_list;       // for transfers longer than two pages
    uint64_t prp_list_phys;
    uint16_t cid;
};

struct ns {
    struct blkdev blk;
    struct ctrl *c;
    uint32_t nsid;
};

static int nctrl;

static uint64_t rd64(struct ctrl *c, uint32_t reg) { return *(volatile uint64_t *)(c->regs + reg); }
static uint32_t rd32(struct ctrl *c, uint32_t reg) { return *(volatile uint32_t *)(c->regs + reg); }
static void wr32(struct ctrl *c, uint32_t reg, uint32_t v) { *(volatile uint32_t *)(c->regs + reg) = v; }
static void wr64(struct ctrl *c, uint32_t reg, uint64_t v) { *(volatile uint64_t *)(c->regs + reg) = v; }

static bool alloc_queue(struct queue *q, uint16_t id)
{
    uint64_t sq = pmm_alloc_zeroed(), cq = pmm_alloc_zeroed();
    if (!sq || !cq)
        return false;
    q->sq = phys_to_virt(sq);
    q->cq = phys_to_virt(cq);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
    q->id = id;
    return true;
}

static uint64_t qphys(const void *p)
{
    return hhdm_to_phys(p);
}

// queues one command and waits for its completion. status 0 is success
static int submit(struct ctrl *c, struct queue *q, struct sqe *cmd)
{
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | (uint32_t)(++c->cid) << 16;
    q->sq[q->sq_tail] = *cmd;
    q->sq_tail = (uint16_t)((q->sq_tail + 1) % QSIZE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr32(c, REG_DB + (2 * q->id) * c->stride, q->sq_tail);

    uint64_t end = timer_uptime_ms() + TIMEOUT_MS;
    for (;;) {
        volatile struct cqe *e = &q->cq[q->cq_head];
        if ((e->status & 1) == q->phase) {
            uint16_t status = e->status >> 1;
            q->cq_head = (uint16_t)((q->cq_head + 1) % QSIZE);
            if (q->cq_head == 0)
                q->phase ^= 1;
            wr32(c, REG_DB + (2 * q->id + 1) * c->stride, q->cq_head);
            return status ? ZK_EIO : 0;
        }
        if (timer_uptime_ms() > end)
            return ZK_EIO;
        __asm__ volatile ("pause");
    }
}

// fills the data pointers for a transfer from the bounce buffer
static void set_prps(struct ctrl *c, struct sqe *cmd, size_t bytes)
{
    cmd->prp1 = c->bounce_phys;
    if (bytes <= PAGE_SIZE)
        cmd->prp2 = 0;
    else if (bytes <= 2 * PAGE_SIZE)
        cmd->prp2 = c->bounce_phys + PAGE_SIZE;
    else {
        size_t pages = (bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        for (size_t i = 1; i < pages; i++)
            c->prp_list[i - 1] = c->bounce_phys + i * PAGE_SIZE;
        cmd->prp2 = c->prp_list_phys;
    }
}

static int rw(struct ns *n, uint64_t lba, uint32_t count, bool write)
{
    struct sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = write ? 0x01 : 0x02;
    cmd.nsid = n->nsid;
    set_prps(n->c, &cmd, (size_t)count * SECTOR_SIZE);
    cmd.cdw10 = (uint32_t)lba;
    cmd.cdw11 = (uint32_t)(lba >> 32);
    cmd.cdw12 = count - 1;
    return submit(n->c, &n->c->io, &cmd);
}

static int nread(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    struct ns *n = d->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t k = count < MAX_SECTORS ? count : MAX_SECTORS;
        int err = rw(n, lba, k, false);
        if (err)
            return err;
        memcpy(out, n->c->bounce, (size_t)k * SECTOR_SIZE);
        out += (size_t)k * SECTOR_SIZE;
        lba += k;
        count -= k;
    }
    return 0;
}

static int nwrite(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct ns *n = d->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t k = count < MAX_SECTORS ? count : MAX_SECTORS;
        memcpy(n->c->bounce, in, (size_t)k * SECTOR_SIZE);
        int err = rw(n, lba, k, true);
        if (err)
            return err;
        in += (size_t)k * SECTOR_SIZE;
        lba += k;
        count -= k;
    }
    return 0;
}

static int identify(struct ctrl *c, uint32_t cns, uint32_t nsid)
{
    struct sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x06;
    cmd.nsid = nsid;
    cmd.prp1 = c->bounce_phys;
    cmd.cdw10 = cns;
    return submit(c, &c->admin, &cmd);
}

static bool wait_ready(struct ctrl *c, bool want)
{
    uint64_t end = timer_uptime_ms() + TIMEOUT_MS;
    while (((rd32(c, REG_CSTS) & 1) != 0) != want) {
        if (timer_uptime_ms() > end)
            return false;
        __asm__ volatile ("pause");
    }
    return true;
}

static void setup(const struct pci_dev *d)
{
    bool is_io;
    uint64_t bar = pci_bar(d, 0, &is_io);
    if (is_io || !bar)
        return;
    pci_enable(d);
    struct ctrl *c = kzalloc(sizeof(*c));
    if (!c)
        return;
    c->regs = vmm_map_mmio(bar, 0x2000);
    uint64_t cap = rd64(c, REG_CAP);
    c->stride = 4U << ((cap >> 32) & 0xF);

    wr32(c, REG_CC, rd32(c, REG_CC) & ~1U);
    if (!wait_ready(c, false))
        goto fail;
    uint64_t bounce = pmm_alloc_pages(BOUNCE_PAGES), prp = pmm_alloc_zeroed();
    if (!c->regs || !bounce || !prp || !alloc_queue(&c->admin, 0) || !alloc_queue(&c->io, 1))
        goto fail;
    c->bounce_phys = bounce;
    c->bounce = phys_to_virt(bounce);
    c->prp_list_phys = prp;
    c->prp_list = phys_to_virt(prp);

    wr32(c, REG_AQA, (QSIZE - 1) << 16 | (QSIZE - 1));
    wr64(c, REG_ASQ, qphys(c->admin.sq));
    wr64(c, REG_ACQ, qphys((void *)c->admin.cq));
    // enabled, 4k pages, nvm command set, 64 byte sq and 16 byte cq entries
    wr32(c, REG_CC, 1 | (6 << 16) | (4 << 20));
    if (!wait_ready(c, true))
        goto fail;

    // io completion queue, then the submission queue that feeds it
    struct sqe cmd;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x05;
    cmd.prp1 = qphys((void *)c->io.cq);
    cmd.cdw10 = (QSIZE - 1) << 16 | 1;
    cmd.cdw11 = 1;    // physically contiguous, no interrupts
    if (submit(c, &c->admin, &cmd))
        goto fail;
    memset(&cmd, 0, sizeof cmd);
    cmd.cdw0 = 0x01;
    cmd.prp1 = qphys(c->io.sq);
    cmd.cdw10 = (QSIZE - 1) << 16 | 1;
    cmd.cdw11 = 1 << 16 | 1;    // completes on queue 1, contiguous
    if (submit(c, &c->admin, &cmd))
        goto fail;

    char model[41] = "";
    if (identify(c, 1, 0) == 0) {
        memcpy(model, c->bounce + 24, 40);
        model[40] = '\0';
        for (int i = 39; i >= 0 && model[i] == ' '; i--)
            model[i] = '\0';
    }
    uint32_t nn = identify(c, 1, 0) == 0 ? *(uint32_t *)(c->bounce + 516) : 1;
    int ctrl_no = nctrl++;
    for (uint32_t nsid = 1; nsid <= nn && nsid <= 8; nsid++) {
        if (identify(c, 0, nsid) != 0)
            continue;
        uint64_t size = *(uint64_t *)c->bounce;
        uint8_t flbas = c->bounce[26] & 0xF;
        uint32_t lbaf = *(uint32_t *)(c->bounce + 128 + flbas * 4);
        uint32_t lbads = (lbaf >> 16) & 0xFF;
        if (!size)
            continue;
        if (lbads != 9) {
            pr_warn("nvme: namespace %u uses %u byte blocks, only 512 is supported\n",
                    nsid, 1U << lbads);
            continue;
        }
        struct ns *n = kzalloc(sizeof(*n));
        if (!n)
            break;
        n->c = c;
        n->nsid = nsid;
        snprintf(n->blk.name, sizeof n->blk.name, "nvme%dn%u", ctrl_no, nsid);
        n->blk.sectors = size;
        n->blk.read = nread;
        n->blk.write = nwrite;
        n->blk.priv = n;
        strlcpy(n->blk.model, model, sizeof n->blk.model);
        blk_register(&n->blk);
        pr_ok("nvme: /dev/%s, %lu MiB, %s\n", n->blk.name, size * SECTOR_SIZE >> 20, model);
        blk_scan_partitions(&n->blk);
    }
    return;
fail:
    pr_warn("nvme: controller at %02x:%02x.%x didn't come up\n", d->bus, d->dev, d->fn);
}

void nvme_init(void)
{
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        if (d->class == 0x01 && d->subclass == 0x08 && d->prog_if == 0x02)
            setup(d);
    }
}
