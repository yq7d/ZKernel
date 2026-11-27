// just enough acpi to power off and reset real machines: the fadt for the
// pm1 control ports and reset register, and \_S5 from the dsdt
#include "arch/x86_64/acpi.h"
#include "arch/x86_64/io.h"
#include "kprintf.h"
#include "lib/string.h"
#include "mm/mm.h"
#include "mm/vmm.h"

static uint32_t pm1a_cnt, pm1b_cnt, smi_cmd;
static uint8_t acpi_enable_val, slp_typa, slp_typb;
static bool have_s5;
static uint8_t reset_space, reset_value;
static uint64_t reset_addr;
static bool have_reset;

static const uint8_t *table(uint64_t phys)
{
    const uint8_t *h = vmm_map_mmio(phys, 36);
    if (!h)
        return NULL;
    uint32_t len = *(const uint32_t *)(h + 4);
    return len >= 36 && len < (16 << 20) ? vmm_map_mmio(phys, len) : NULL;
}

// \_S5_ is a package whose first two elements are the sleep type values
static void find_s5(const uint8_t *dsdt)
{
    uint32_t len = *(const uint32_t *)(dsdt + 4);
    for (uint32_t i = 36; i + 8 < len; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) != 0 || dsdt[i + 4] != 0x12)
            continue;
        const uint8_t *p = dsdt + i + 5;
        p += ((*p >> 6) & 3) + 1;    // package length
        p++;                         // element count
        uint8_t vals[2];
        for (int k = 0; k < 2; k++) {
            if (*p == 0x0A) {        // byte prefix
                vals[k] = p[1];
                p += 2;
            } else {                 // zero, one, or a small constant
                vals[k] = *p == 0x01 ? 1 : *p == 0xFF ? 0 : *p;
                p++;
            }
        }
        slp_typa = vals[0];
        slp_typb = vals[1];
        have_s5 = true;
        return;
    }
}

static void parse_fadt(const uint8_t *f)
{
    uint32_t len = *(const uint32_t *)(f + 4);
    smi_cmd = *(const uint32_t *)(f + 48);
    acpi_enable_val = f[52];
    pm1a_cnt = *(const uint32_t *)(f + 64);
    pm1b_cnt = *(const uint32_t *)(f + 68);
    uint64_t dsdt = *(const uint32_t *)(f + 40);
    if (len >= 148 && *(const uint64_t *)(f + 140))
        dsdt = *(const uint64_t *)(f + 140);
    if (len >= 129 && (*(const uint32_t *)(f + 112) & (1 << 10))) {
        reset_space = f[116];
        reset_addr = *(const uint64_t *)(f + 120);
        reset_value = f[128];
        have_reset = reset_addr != 0;
    }
    const uint8_t *d = dsdt ? table(dsdt) : NULL;
    if (d && memcmp(d, "DSDT", 4) == 0)
        find_s5(d);
}

void acpi_init(uint64_t rsdp_phys)
{
    const uint8_t *rsdp = rsdp_phys ? vmm_map_mmio(rsdp_phys, 36) : NULL;
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8) != 0) {
        pr_warn("acpi: no rsdp\n");
        return;
    }
    bool x = rsdp[15] >= 2 && *(const uint64_t *)(rsdp + 24);
    const uint8_t *root = table(x ? *(const uint64_t *)(rsdp + 24) : *(const uint32_t *)(rsdp + 16));
    if (!root)
        return;
    uint32_t len = *(const uint32_t *)(root + 4);
    size_t esize = x ? 8 : 4;
    for (size_t off = 36; off + esize <= len; off += esize) {
        uint64_t phys = x ? *(const uint64_t *)(root + off) : *(const uint32_t *)(root + off);
        const uint8_t *t = table(phys);
        if (t && memcmp(t, "FACP", 4) == 0) {
            parse_fadt(t);
            break;
        }
    }
    pr_ok("acpi: pm1a %#x, s5 %s, reset register %s\n", pm1a_cnt, have_s5 ? "found" : "missing",
          have_reset ? "yes" : "no");
}

bool acpi_poweroff(void)
{
    if (!pm1a_cnt || !have_s5)
        return false;
    // the firmware may still own acpi, ask for it
    if (!(inw((uint16_t)pm1a_cnt) & 1) && smi_cmd && acpi_enable_val) {
        outb((uint16_t)smi_cmd, acpi_enable_val);
        for (int i = 0; i < 300 && !(inw((uint16_t)pm1a_cnt) & 1); i++)
            for (volatile int k = 0; k < 100000; k++)
                ;
    }
    outw((uint16_t)pm1a_cnt, (uint16_t)(slp_typa << 10 | 1 << 13));
    if (pm1b_cnt)
        outw((uint16_t)pm1b_cnt, (uint16_t)(slp_typb << 10 | 1 << 13));
    return false;    // still here, so it didn't work
}

void acpi_reset(void)
{
    if (!have_reset)
        return;
    if (reset_space == 1) {
        outb((uint16_t)reset_addr, reset_value);
    } else if (reset_space == 0) {
        volatile uint8_t *r = vmm_map_mmio(reset_addr, 1);
        if (r)
            *r = reset_value;
    }
}
