#include "arch/x86_64/pic.h"
#include "arch/x86_64/io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1

#define ICW1_INIT 0x10
#define ICW1_ICW4 0x01
#define ICW4_8086 0x01
#define OCW3_READ_ISR 0x0B
#define PIC_EOI   0x20

void pic_init(uint8_t vector_base)
{
    outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4); io_wait();
    outb(PIC1_DATA, vector_base);          io_wait();  /* IRQ 0-7  */
    outb(PIC2_DATA, vector_base + 8);      io_wait();  /* IRQ 8-15 */
    outb(PIC1_DATA, 1 << 2);               io_wait();  /* slave on IRQ2 */
    outb(PIC2_DATA, 2);                    io_wait();  /* slave identity */
    outb(PIC1_DATA, ICW4_8086);            io_wait();
    outb(PIC2_DATA, ICW4_8086);            io_wait();

    /* mask everything but the cascade line */
    outb(PIC1_DATA, 0xFF & ~(1 << 2));
    outb(PIC2_DATA, 0xFF);
}

void pic_mask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) | (1 << (irq & 7)));
}

void pic_unmask(uint8_t irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    outb(port, inb(port) & ~(1 << (irq & 7)));
}

void pic_eoi(uint8_t irq)
{
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* irq 7/15 can be spurious, real ones have their ISR bit set */
bool pic_is_spurious(uint8_t irq)
{
    if (irq == 7) {
        outb(PIC1_CMD, OCW3_READ_ISR);
        return !(inb(PIC1_CMD) & 0x80);
    }
    if (irq == 15) {
        outb(PIC2_CMD, OCW3_READ_ISR);
        if (!(inb(PIC2_CMD) & 0x80)) {
            outb(PIC1_CMD, PIC_EOI);    /* master still needs one */
            return true;
        }
    }
    return false;
}
