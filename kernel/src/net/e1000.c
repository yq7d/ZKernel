// intel gigabit cards: 8254x (e1000) and 82574 (e1000e, qemu's q35 default),
// legacy descriptors, polled like the virtio card
#include "net/net.h"
#include "drivers/pci.h"
#include "drivers/pit.h"
#include "arch/x86_64/cpu.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "mm/pmm.h"
#include "mm/vmm.h"

#define CTRL   0x0000
#define STATUS 0x0008
#define EERD   0x0014
#define ICR    0x00C0
#define IMC    0x00D8
#define RCTL   0x0100
#define TCTL   0x0400
#define TIPG   0x0410
#define RDBAL  0x2800
#define RDBAH  0x2804
#define RDLEN  0x2808
#define RDH    0x2810
#define RDT    0x2818
#define TDBAL  0x3800
#define TDBAH  0x3804
#define TDLEN  0x3808
#define TDH    0x3810
#define TDT    0x3818
#define MTA    0x5200
#define RAL    0x5400
#define RAH    0x5404

#define CTRL_SLU  (1U << 6)
#define CTRL_ASDE (1U << 5)
#define CTRL_RST  (1U << 26)
#define RCTL_EN   (1U << 1)
#define RCTL_BAM  (1U << 15)
#define RCTL_SECRC (1U << 26)
#define TCTL_EN   (1U << 1)
#define TCTL_PSP  (1U << 3)

#define NRX 256    // a full page of descriptors
#define NTX 16
#define BUF 2048

struct rx_desc {
    uint64_t addr;
    uint16_t length, csum;
    volatile uint8_t status;
    uint8_t errors;
    uint16_t special;
};

struct tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso, cmd;
    volatile uint8_t status;
    uint8_t css;
    uint16_t special;
};

static volatile uint8_t *regs;
static struct rx_desc *rx;
static struct tx_desc *tx;
static uint8_t *rx_bufs, *tx_bufs;
static uint64_t rx_phys, tx_phys;
static uint32_t rx_next, tx_next;

static const uint16_t ids[] = {
    0x100E, 0x100F, 0x1004, 0x100C, 0x1010, 0x1015, 0x1019, 0x101E, 0x1026, 0x1076,
    0x107C, 0x10D3, 0x10F6,
};

static uint32_t rd(uint32_t r) { return *(volatile uint32_t *)(regs + r); }
static void wr(uint32_t r, uint32_t v) { *(volatile uint32_t *)(regs + r) = v; }

static int send(const void *frame, size_t len)
{
    if (len > BUF)
        return ZK_EINVAL;
    uint64_t flags = irq_save();
    struct tx_desc *d = &tx[tx_next];
    // wait for this slot's last frame to have gone out
    for (int spin = 0; spin < 1000000 && d->cmd && !(d->status & 1); spin++)
        __asm__ volatile ("pause");
    memcpy(tx_bufs + (size_t)tx_next * BUF, frame, len);
    d->addr = tx_phys + (uint64_t)tx_next * BUF;
    d->length = (uint16_t)len;
    d->cmd = 0x0B;    // end of packet, insert fcs, report status
    d->status = 0;
    tx_next = (tx_next + 1) % NTX;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    wr(TDT, tx_next);
    irq_restore(flags);
    return 0;
}

static int poll(void)
{
    int n = 0;
    for (;;) {
        struct rx_desc *d = &rx[rx_next];
        if (!(d->status & 1))
            break;
        if ((d->status & 2) && !d->errors)    // whole frame, no errors
            net_input(rx_bufs + (size_t)rx_next * BUF, d->length);
        d->status = 0;
        wr(RDT, rx_next);
        rx_next = (rx_next + 1) % NRX;
        n++;
    }
    return n;
}

static bool pending(void)
{
    return rx && (rx[rx_next].status & 1);
}

static uint16_t eeprom(uint8_t addr)
{
    wr(EERD, 1 | (uint32_t)addr << 8);
    for (int i = 0; i < 100000; i++) {
        uint32_t v = rd(EERD);
        if (v & (1 << 4))
            return (uint16_t)(v >> 16);
    }
    return 0;
}

static const struct nic_ops ops = { "e1000", send, poll, pending };

bool e1000_init(void)
{
    const struct pci_dev *dev = NULL;
    for (size_t i = 0; i < sizeof ids / sizeof ids[0] && !dev; i++)
        dev = pci_find(0x8086, ids[i], NULL);
    if (!dev)
        return false;
    bool is_io;
    uint64_t bar = pci_bar(dev, 0, &is_io);
    if (is_io || !bar)
        return false;
    pci_enable(dev);
    regs = vmm_map_mmio(bar, 0x20000);
    if (!regs)
        return false;

    wr(IMC, 0xFFFFFFFF);
    wr(CTRL, rd(CTRL) | CTRL_RST);
    for (volatile int i = 0; i < 100000; i++)
        ;
    wr(IMC, 0xFFFFFFFF);
    rd(ICR);
    wr(CTRL, (rd(CTRL) | CTRL_SLU | CTRL_ASDE) & ~(1U << 3));

    // the address is usually already in the receive address registers
    uint32_t lo = rd(RAL), hi = rd(RAH);
    if (lo || (hi & 0xFFFF)) {
        for (int i = 0; i < 4; i++)
            netif.mac[i] = (uint8_t)(lo >> (8 * i));
        netif.mac[4] = (uint8_t)hi;
        netif.mac[5] = (uint8_t)(hi >> 8);
    } else {
        for (int i = 0; i < 3; i++) {
            uint16_t w = eeprom((uint8_t)i);
            netif.mac[i * 2] = (uint8_t)w;
            netif.mac[i * 2 + 1] = (uint8_t)(w >> 8);
        }
        wr(RAL, (uint32_t)netif.mac[0] | (uint32_t)netif.mac[1] << 8 |
                (uint32_t)netif.mac[2] << 16 | (uint32_t)netif.mac[3] << 24);
        wr(RAH, (uint32_t)netif.mac[4] | (uint32_t)netif.mac[5] << 8 | (1U << 31));
    }
    for (int i = 0; i < 128; i++)
        wr(MTA + i * 4, 0);

    uint64_t rmem = pmm_alloc_zeroed(), tmem = pmm_alloc_zeroed();
    rx_phys = pmm_alloc_pages(NRX * BUF / PAGE_SIZE);
    tx_phys = pmm_alloc_pages(NTX * BUF / PAGE_SIZE);
    if (!rmem || !tmem || !rx_phys || !tx_phys)
        return false;
    rx = phys_to_virt(rmem);
    tx = phys_to_virt(tmem);
    rx_bufs = phys_to_virt(rx_phys);
    tx_bufs = phys_to_virt(tx_phys);
    for (int i = 0; i < NRX; i++)
        rx[i].addr = rx_phys + (uint64_t)i * BUF;

    wr(RDBAL, (uint32_t)rmem);
    wr(RDBAH, (uint32_t)(rmem >> 32));
    wr(RDLEN, NRX * sizeof(struct rx_desc));
    wr(RDH, 0);
    wr(RDT, NRX - 1);
    wr(RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);    // 2048 byte buffers

    wr(TDBAL, (uint32_t)tmem);
    wr(TDBAH, (uint32_t)(tmem >> 32));
    wr(TDLEN, NTX * sizeof(struct tx_desc));
    wr(TDH, 0);
    wr(TDT, 0);
    wr(TCTL, TCTL_EN | TCTL_PSP | (15 << 4) | (64 << 12));
    wr(TIPG, 10 | (8 << 10) | (6 << 20));

    nic = &ops;
    return true;
}
