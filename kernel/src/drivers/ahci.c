// sata disks through an ahci controller. one command at a time per port,
// completion is polled. disks show up as /dev/sda, /dev/sdb...
#include "drivers/ahci.h"
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

#define HBA_GHC  0x04
#define HBA_PI   0x0C
#define GHC_AE   (1U << 31)

#define PORT_BASE 0x100
#define PORT_SIZE 0x80
#define P_CLB   0x00
#define P_CLBU  0x04
#define P_FB    0x08
#define P_FBU   0x0C
#define P_IS    0x10
#define P_CMD   0x18
#define P_TFD   0x20
#define P_SIG   0x24
#define P_SSTS  0x28
#define P_SERR  0x30
#define P_CI    0x38

#define CMD_ST  (1U << 0)
#define CMD_FRE (1U << 4)
#define CMD_FR  (1U << 14)
#define CMD_CR  (1U << 15)

#define SIG_ATA 0x00000101
#define TFD_ERR 0x01
#define TFD_BSY 0x80
#define TFD_DRQ 0x08

#define ATA_IDENTIFY  0xEC
#define ATA_READ_EXT  0x25
#define ATA_WRITE_EXT 0x35
#define ATA_FLUSH_EXT 0xEA

#define BOUNCE_PAGES 16
#define MAX_SECTORS  (BOUNCE_PAGES * PAGE_SIZE / SECTOR_SIZE)
#define TIMEOUT_MS   5000

struct cmd_header {
    uint16_t flags;    // cfl in the low 5 bits, W is bit 6
    uint16_t prdtl;
    volatile uint32_t prdbc;
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
};

struct prdt {
    uint32_t dba, dbau, reserved, dbc;
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prdt prdt[1];
};

struct port {
    struct blkdev blk;
    volatile uint8_t *regs;
    struct cmd_header *list;
    struct cmd_table *table;
    uint64_t table_phys;
    uint8_t *bounce;
    uint64_t bounce_phys;
};

static int ndisks;

static uint32_t rd(struct port *p, uint32_t reg)
{
    return *(volatile uint32_t *)(p->regs + reg);
}

static void wrr(struct port *p, uint32_t reg, uint32_t v)
{
    *(volatile uint32_t *)(p->regs + reg) = v;
}

static bool wait_clear(struct port *p, uint32_t reg, uint32_t bits)
{
    uint64_t end = timer_uptime_ms() + TIMEOUT_MS;
    while (rd(p, reg) & bits) {
        if (timer_uptime_ms() > end)
            return false;
        __asm__ volatile ("pause");
    }
    return true;
}

static void stop(struct port *p)
{
    wrr(p, P_CMD, rd(p, P_CMD) & ~CMD_ST);
    wait_clear(p, P_CMD, CMD_CR);
    wrr(p, P_CMD, rd(p, P_CMD) & ~CMD_FRE);
    wait_clear(p, P_CMD, CMD_FR);
}

static void start(struct port *p)
{
    wait_clear(p, P_CMD, CMD_CR);
    wrr(p, P_CMD, rd(p, P_CMD) | CMD_FRE);
    wrr(p, P_CMD, rd(p, P_CMD) | CMD_ST);
}

// runs one command in slot 0 against the bounce buffer
static int issue(struct port *p, uint8_t command, uint64_t lba, uint32_t count, bool write, size_t bytes)
{
    if (!wait_clear(p, P_TFD, TFD_BSY | TFD_DRQ))
        return ZK_EIO;
    struct cmd_header *h = &p->list[0];
    h->flags = (uint16_t)(5 | (write ? 1 << 6 : 0));    // fis is 5 dwords
    h->prdtl = bytes ? 1 : 0;
    h->prdbc = 0;
    memset(p->table, 0, sizeof *p->table);
    p->table->prdt[0].dba = (uint32_t)p->bounce_phys;
    p->table->prdt[0].dbau = (uint32_t)(p->bounce_phys >> 32);
    p->table->prdt[0].dbc = (uint32_t)(bytes - 1);

    uint8_t *f = p->table->cfis;
    f[0] = 0x27;    // host to device
    f[1] = 0x80;    // this is a command
    f[2] = command;
    f[4] = (uint8_t)lba;
    f[5] = (uint8_t)(lba >> 8);
    f[6] = (uint8_t)(lba >> 16);
    f[7] = 0x40;    // lba mode
    f[8] = (uint8_t)(lba >> 24);
    f[9] = (uint8_t)(lba >> 32);
    f[10] = (uint8_t)(lba >> 40);
    f[12] = (uint8_t)count;
    f[13] = (uint8_t)(count >> 8);

    wrr(p, P_IS, 0xFFFFFFFF);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wrr(p, P_CI, 1);
    if (!wait_clear(p, P_CI, 1) || (rd(p, P_TFD) & TFD_ERR)) {
        // get the port going again after an error
        stop(p);
        wrr(p, P_SERR, 0xFFFFFFFF);
        start(p);
        return ZK_EIO;
    }
    return 0;
}

static int sread(struct blkdev *d, uint64_t lba, uint32_t count, void *buf)
{
    struct port *p = d->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        int err = issue(p, ATA_READ_EXT, lba, n, false, n * SECTOR_SIZE);
        if (err)
            return err;
        memcpy(out, p->bounce, n * SECTOR_SIZE);
        out += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static int swrite(struct blkdev *d, uint64_t lba, uint32_t count, const void *buf)
{
    struct port *p = d->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = count < MAX_SECTORS ? count : MAX_SECTORS;
        memcpy(p->bounce, in, n * SECTOR_SIZE);
        int err = issue(p, ATA_WRITE_EXT, lba, n, true, n * SECTOR_SIZE);
        if (err)
            return err;
        in += n * SECTOR_SIZE;
        lba += n;
        count -= n;
    }
    return 0;
}

static void ata_string(const uint16_t *words, int n, char *out)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        out[k++] = (char)(words[i] >> 8);
        out[k++] = (char)words[i];
    }
    while (k && out[k - 1] == ' ')
        k--;
    out[k] = '\0';
}

static void setup_port(volatile uint8_t *regs)
{
    struct port *p = kzalloc(sizeof(*p));
    uint64_t mem = pmm_alloc_zeroed();          // command list 1k, fis 256, table
    uint64_t bounce = pmm_alloc_pages(BOUNCE_PAGES);
    if (!p || !mem || !bounce) {
        kfree(p);
        return;
    }
    p->regs = regs;
    p->list = phys_to_virt(mem);
    p->table_phys = mem + 2048;
    p->table = phys_to_virt(p->table_phys);
    p->bounce_phys = bounce;
    p->bounce = phys_to_virt(bounce);

    stop(p);
    wrr(p, P_CLB, (uint32_t)mem);
    wrr(p, P_CLBU, (uint32_t)(mem >> 32));
    wrr(p, P_FB, (uint32_t)(mem + 1024));
    wrr(p, P_FBU, (uint32_t)((mem + 1024) >> 32));
    p->list[0].ctba = (uint32_t)p->table_phys;
    p->list[0].ctbau = (uint32_t)(p->table_phys >> 32);
    wrr(p, P_SERR, 0xFFFFFFFF);
    wrr(p, P_IS, 0xFFFFFFFF);
    start(p);

    if (issue(p, ATA_IDENTIFY, 0, 0, false, 512) != 0) {
        pr_warn("ahci: a disk didn't answer identify\n");
        return;
    }
    uint16_t id[256];
    memcpy(id, p->bounce, 512);
    uint64_t sectors = (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 |
                       (uint64_t)id[103] << 48;
    if (!(id[83] & (1 << 10)) || !sectors) {
        pr_warn("ahci: a disk without 48 bit lba, skipping\n");
        return;
    }
    char model[41];
    ata_string(id + 27, 20, model);

    snprintf(p->blk.name, sizeof p->blk.name, "sd%c", 'a' + ndisks++);
    p->blk.sectors = sectors;
    p->blk.read = sread;
    p->blk.write = swrite;
    p->blk.priv = p;
    strlcpy(p->blk.model, model, sizeof p->blk.model);
    blk_register(&p->blk);
    pr_ok("ahci: /dev/%s, %lu MiB, %s\n", p->blk.name, sectors * SECTOR_SIZE >> 20, model);
    blk_scan_partitions(&p->blk);
}

void ahci_init(void)
{
    for (int i = 0; i < pci_count(); i++) {
        const struct pci_dev *d = pci_get(i);
        if (d->class != 0x01 || d->subclass != 0x06 || d->prog_if != 0x01)
            continue;
        bool is_io;
        uint64_t bar = pci_bar(d, 5, &is_io);
        if (is_io || !bar)
            continue;
        pci_enable(d);
        volatile uint8_t *hba = vmm_map_mmio(bar, 0x1100);
        if (!hba)
            continue;
        *(volatile uint32_t *)(hba + HBA_GHC) |= GHC_AE;
        uint32_t pi = *(volatile uint32_t *)(hba + HBA_PI);
        for (int port = 0; port < 32; port++) {
            if (!(pi & (1U << port)))
                continue;
            volatile uint8_t *regs = hba + PORT_BASE + port * PORT_SIZE;
            uint32_t ssts = *(volatile uint32_t *)(regs + P_SSTS);
            // something is attached and the link is up, and it's a disk
            if ((ssts & 0xF) != 3 || ((ssts >> 8) & 0xF) != 1)
                continue;
            if (*(volatile uint32_t *)(regs + P_SIG) != SIG_ATA)
                continue;
            setup_port(regs);
        }
    }
}
