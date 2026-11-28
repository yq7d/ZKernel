#pragma once
#include <stdbool.h>
#include <stdint.h>

struct pci_dev {
    uint8_t bus, dev, fn;
    uint16_t vendor, device;
    uint8_t class, subclass, prog_if;
    uint8_t irq;
};

void pci_init(void);
int pci_count(void);
const struct pci_dev *pci_get(int i);
// next device after *from matching vendor/device, from NULL starts at the beginning
const struct pci_dev *pci_find(uint16_t vendor, uint16_t device, const struct pci_dev *from);

uint32_t pci_read32(const struct pci_dev *d, uint8_t off);
uint16_t pci_read16(const struct pci_dev *d, uint8_t off);
void pci_write16(const struct pci_dev *d, uint8_t off, uint16_t v);
void pci_write32(const struct pci_dev *d, uint8_t off, uint32_t v);
// io or memory base of a bar, with the flag bits stripped
uint64_t pci_bar(const struct pci_dev *d, int bar, bool *is_io);
void pci_enable(const struct pci_dev *d);    // io, memory and bus mastering
const char *pci_class_name(uint8_t class, uint8_t subclass);
