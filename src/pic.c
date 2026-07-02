/* 8259A PIC remap + EOI (milestone-4). */
#include "pic.h"
#include "io.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define ICW1_INIT 0x11        /* init + expect ICW4 */
#define ICW4_8086 0x01        /* 8086/88 mode */
#define PIC_EOI   0x20

static void io_wait(void) { outb(0x80, 0); }

void pic_init(uint16_t unmask) {
    /* Standard 4-word init sequence, both chips. */
    outb(PIC1_CMD, ICW1_INIT); io_wait();
    outb(PIC2_CMD, ICW1_INIT); io_wait();
    outb(PIC1_DATA, 32);       io_wait();   /* master -> vectors 32-39 */
    outb(PIC2_DATA, 40);       io_wait();   /* slave  -> vectors 40-47 */
    outb(PIC1_DATA, 0x04);     io_wait();   /* master: slave on IRQ2   */
    outb(PIC2_DATA, 0x02);     io_wait();   /* slave: cascade identity */
    outb(PIC1_DATA, ICW4_8086); io_wait();
    outb(PIC2_DATA, ICW4_8086); io_wait();

    /* Masks: 0 = enabled. Keep IRQ2 (cascade) open if any slave IRQ is
     * wanted; here we only use master IRQs 0/1, so mask the whole slave. */
    outb(PIC1_DATA, (uint8_t)~(unmask & 0xFF));
    outb(PIC2_DATA, (uint8_t)~((unmask >> 8) & 0xFF));
}

void pic_eoi(int irq) {
    if (irq >= 8) outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}
