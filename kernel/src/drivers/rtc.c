// cmos real time clock, read once at boot. after that time comes from the pit
#include "drivers/rtc.h"
#include "arch/x86_64/cpu.h"
#include "arch/x86_64/io.h"
#include "drivers/pit.h"
#include "lib/time.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static int64_t boot_epoch;

static uint8_t cmos(uint8_t reg)
{
    outb(CMOS_ADDR, reg | 0x80);    // keep nmi disabled
    return inb(CMOS_DATA);
}

static bool updating(void)
{
    return cmos(0x0A) & 0x80;
}

static int bcd(int v)
{
    return (v & 0x0F) + (v >> 4) * 10;
}

void rtc_init(void)
{
    uint64_t flags = irq_save();
    uint8_t r[7], prev[7];
    // read until two reads in a row agree, so an update can't tear it
    for (int tries = 0; tries < 10; tries++) {
        while (updating())
            ;
        uint8_t regs[7] = { 0x00, 0x02, 0x04, 0x07, 0x08, 0x09, 0x32 };
        for (int i = 0; i < 7; i++)
            r[i] = cmos(regs[i]);
        if (tries && __builtin_memcmp(r, prev, sizeof r) == 0)
            break;
        __builtin_memcpy(prev, r, sizeof r);
    }
    uint8_t b = cmos(0x0B);
    irq_restore(flags);

    int sec = r[0], min = r[1], hour = r[2], day = r[3], mon = r[4], year = r[5], cent = r[6];
    bool pm = hour & 0x80;
    hour &= 0x7F;
    if (!(b & 0x04)) {
        sec = bcd(sec); min = bcd(min); hour = bcd(hour);
        day = bcd(day); mon = bcd(mon); year = bcd(year); cent = bcd(cent);
    }
    if (!(b & 0x02) && pm)
        hour = (hour % 12) + 12;
    int full = (cent >= 19 && cent <= 30 ? cent : 20) * 100 + year;

    struct datetime dt = { .year = full, .month = mon, .day = day,
                           .hour = hour, .min = min, .sec = sec };
    boot_epoch = datetime_to_epoch(&dt) - (int64_t)(timer_uptime_ms() / 1000);
}

int64_t rtc_now(void)
{
    return boot_epoch + (int64_t)(timer_uptime_ms() / 1000);
}
