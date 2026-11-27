// 8042 ps/2 controller
#include "drivers/ps2.h"
#include "arch/x86_64/io.h"

#define PS2_STATUS 0x64
#define PS2_CMD    0x64

#define STATUS_OUT_FULL 0x01
#define STATUS_IN_FULL  0x02

#define CMD_READ_CONFIG  0x20
#define CMD_WRITE_CONFIG 0x60
#define CMD_DISABLE_P2   0xA7
#define CMD_DISABLE_P1   0xAD
#define CMD_ENABLE_P1    0xAE
#define CMD_SELF_TEST    0xAA
#define CMD_TEST_P1      0xAB
#define CMD_RESET_CPU    0xFE

#define CFG_P1_IRQ       0x01
#define CFG_P2_IRQ       0x02
#define CFG_P1_CLOCK_OFF 0x10
#define CFG_TRANSLATE    0x40

#define DEV_ENABLE_SCAN  0xF4
#define DEV_ACK          0xFA

#define TIMEOUT 100000

static bool wait_write(void)
{
    for (int i = 0; i < TIMEOUT; i++)
        if (!(inb(PS2_STATUS) & STATUS_IN_FULL))
            return true;
    return false;
}

static bool wait_read(void)
{
    for (int i = 0; i < TIMEOUT; i++)
        if (inb(PS2_STATUS) & STATUS_OUT_FULL)
            return true;
    return false;
}

static bool send_cmd(uint8_t c)
{
    if (!wait_write())
        return false;
    outb(PS2_CMD, c);
    return true;
}

static bool send_data(uint8_t d)
{
    if (!wait_write())
        return false;
    outb(PS2_DATA, d);
    return true;
}

static int read_data(void)
{
    if (!wait_read())
        return -1;
    return inb(PS2_DATA);
}

static void flush(void)
{
    for (int i = 0; i < 64 && (inb(PS2_STATUS) & STATUS_OUT_FULL); i++)
        inb(PS2_DATA);
}

static bool write_config(uint8_t cfg)
{
    return send_cmd(CMD_WRITE_CONFIG) && send_data(cfg);
}

bool ps2_init(void)
{
    // floating bus, no controller
    if (inb(PS2_STATUS) == 0xFF)
        return false;

    send_cmd(CMD_DISABLE_P1);
    send_cmd(CMD_DISABLE_P2);
    flush();

    if (!send_cmd(CMD_READ_CONFIG))
        return false;
    int cfg = read_data();
    if (cfg < 0)
        return false;

    cfg &= ~(CFG_P1_IRQ | CFG_P2_IRQ);
    cfg |= CFG_TRANSLATE;
    if (!write_config((uint8_t)cfg))
        return false;

    if (!send_cmd(CMD_SELF_TEST) || read_data() != 0x55)
        return false;
    write_config((uint8_t)cfg);    // self test resets it on some hardware

    if (!send_cmd(CMD_TEST_P1) || read_data() != 0x00)
        return false;

    send_cmd(CMD_ENABLE_P1);
    send_data(DEV_ENABLE_SCAN);
    if (read_data() != DEV_ACK)
        flush();

    cfg |= CFG_P1_IRQ;
    cfg &= ~CFG_P1_CLOCK_OFF;
    return write_config((uint8_t)cfg);
}

#define CMD_ENABLE_P2    0xA8
#define CMD_WRITE_P2     0xD4
#define CFG_P2_CLOCK_OFF 0x20

static bool mouse_cmd(uint8_t c)
{
    if (!send_cmd(CMD_WRITE_P2) || !send_data(c))
        return false;
    return read_data() == DEV_ACK;
}

bool ps2_mouse_init(void)
{
    send_cmd(CMD_ENABLE_P2);
    if (!send_cmd(CMD_READ_CONFIG))
        return false;
    int cfg = read_data();
    if (cfg < 0 || (cfg & CFG_P2_CLOCK_OFF))
        return false;    // no second port
    flush();
    if (!mouse_cmd(0xF6) || !mouse_cmd(0xF4))    // defaults, then start reporting
        return false;
    cfg |= CFG_P2_IRQ;
    return write_config((uint8_t)cfg);
}

void ps2_reboot(void)
{
    wait_write();
    outb(PS2_CMD, CMD_RESET_CPU);
}
