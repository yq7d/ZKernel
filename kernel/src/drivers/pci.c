// pci config space through the legacy 0xcf8/0xcfc ports
#include "drivers/pci.h"
#include <stddef.h>
#include "arch/x86_64/io.h"

#define CONFIG_ADDR 0xCF8
#define CONFIG_DATA 0xCFC
#define MAX_DEVS    64

static struct pci_dev devs[MAX_DEVS];
static int ndevs;

static uint32_t cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off)
{
    outl(CONFIG_ADDR, (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                      ((uint32_t)fn << 8) | (off & 0xFC));
    return inl(CONFIG_DATA);
}

static void cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint8_t off, uint32_t v)
{
    outl(CONFIG_ADDR, (1U << 31) | ((uint32_t)bus << 16) | ((uint32_t)dev << 11) |
                      ((uint32_t)fn << 8) | (off & 0xFC));
    outl(CONFIG_DATA, v);
}

uint32_t pci_read32(const struct pci_dev *d, uint8_t off)
{
    return cfg_read(d->bus, d->dev, d->fn, off);
}

uint16_t pci_read16(const struct pci_dev *d, uint8_t off)
{
    return (uint16_t)(pci_read32(d, off) >> ((off & 2) * 8));
}

void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v)
{
    uint32_t old = pci_read32(d, off);
    int shift = (off & 2) * 8;
    old = (old & ~(0xFFFFU << shift)) | ((uint32_t)v << shift);
    cfg_write(d->bus, d->dev, d->fn, off, old);
}

uint64_t pci_bar(const struct pci_dev *d, int bar, bool *is_io)
{
    uint32_t v = pci_read32(d, (uint8_t)(0x10 + bar * 4));
    if (v & 1) {
        *is_io = true;
        return v & ~3U;
    }
    *is_io = false;
    uint64_t base = v & ~0xFU;
    if (((v >> 1) & 3) == 2)    // 64 bit
        base |= (uint64_t)pci_read32(d, (uint8_t)(0x14 + bar * 4)) << 32;
    return base;
}

void pci_enable(const struct pci_dev *d)
{
    pci_write16(d, 0x04, pci_read16(d, 0x04) | 0x7);
}

static void probe(uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t id = cfg_read(bus, dev, fn, 0);
    if ((id & 0xFFFF) == 0xFFFF || ndevs == MAX_DEVS)
        return;
    uint32_t cls = cfg_read(bus, dev, fn, 0x08);
    struct pci_dev *d = &devs[ndevs++];
    d->bus = bus;
    d->dev = dev;
    d->fn = fn;
    d->vendor = (uint16_t)id;
    d->device = (uint16_t)(id >> 16);
    d->class = (uint8_t)(cls >> 24);
    d->subclass = (uint8_t)(cls >> 16);
    d->prog_if = (uint8_t)(cls >> 8);
    d->irq = (uint8_t)cfg_read(bus, dev, fn, 0x3C);
}

void pci_init(void)
{
    for (int bus = 0; bus < 256; bus++) {
        for (uint8_t dev = 0; dev < 32; dev++) {
            if ((cfg_read((uint8_t)bus, dev, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            bool multi = (cfg_read((uint8_t)bus, dev, 0, 0x0C) >> 16) & 0x80;
            for (uint8_t fn = 0; fn < (multi ? 8 : 1); fn++)
                probe((uint8_t)bus, dev, fn);
        }
    }
}

int pci_count(void)
{
    return ndevs;
}

const struct pci_dev *pci_get(int i)
{
    return i >= 0 && i < ndevs ? &devs[i] : NULL;
}

const struct pci_dev *pci_find(uint16_t vendor, uint16_t device, const struct pci_dev *from)
{
    int start = from ? (int)(from - devs) + 1 : 0;
    for (int i = start; i < ndevs; i++)
        if (devs[i].vendor == vendor && devs[i].device == device)
            return &devs[i];
    return NULL;
}

const char *pci_class_name(uint8_t class, uint8_t subclass)
{
    switch (class) {
    case 0x01:
        switch (subclass) {
        case 0x01: return "IDE controller";
        case 0x06: return "SATA controller";
        case 0x08: return "NVMe controller";
        default:   return "storage controller";
        }
    case 0x02: return "network controller";
    case 0x03: return "display controller";
    case 0x04: return "multimedia controller";
    case 0x06:
        switch (subclass) {
        case 0x00: return "host bridge";
        case 0x01: return "ISA bridge";
        case 0x04: return "PCI bridge";
        default:   return "bridge";
        }
    case 0x0C:
        switch (subclass) {
        case 0x03: return "USB controller";
        case 0x05: return "SMBus controller";
        default:   return "serial bus controller";
        }
    default:   return "device";
    }
}
