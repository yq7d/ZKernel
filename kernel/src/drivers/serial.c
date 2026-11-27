/* com1, 16550 uart */
#include "drivers/serial.h"
#include "arch/x86_64/io.h"

#define COM1 0x3F8

bool serial_init(void)
{
    outb(COM1 + 1, 0x00); /* no interrupts */
    outb(COM1 + 3, 0x80); /* dlab */
    outb(COM1 + 0, 0x01); /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); /* 8n1 */
    outb(COM1 + 2, 0xC7); /* fifo */
    outb(COM1 + 4, 0x1E); /* loopback test */

    outb(COM1 + 0, 0xAE);
    if (inb(COM1 + 0) != 0xAE)
        return false;

    outb(COM1 + 4, 0x0F);
    return true;
}

void serial_putc(char c)
{
    if (c == '\n')
        serial_putc('\r');
    while (!(inb(COM1 + 5) & 0x20))
        ;
    outb(COM1, (uint8_t)c);
}

void serial_puts(const char *s)
{
    while (*s)
        serial_putc(*s++);
}

void serial_puthex(unsigned long v)
{
    serial_puts("0x");
    for (int i = 60; i >= 0; i -= 4)
        serial_putc("0123456789abcdef"[(v >> i) & 0xF]);
}

void serial_putdec(unsigned long v)
{
    char buf[21];
    int i = 0;
    do {
        buf[i++] = '0' + v % 10;
        v /= 10;
    } while (v);
    while (i--)
        serial_putc(buf[i]);
}
