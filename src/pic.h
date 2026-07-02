/* 8259A PIC — remap the two chips off the CPU exception vectors (0-31)
 * to 32-47 so hardware IRQs don't alias faults, and drive end-of-interrupt.
 * (The APIC is a later concern; the legacy PIC is all M4 needs.) */
#ifndef EIGENOS_PIC_H
#define EIGENOS_PIC_H

#include <stdint.h>

/* Remap to vectors 32-47 and set the IRQ mask: only `unmask` bits are
 * enabled (bit i = IRQ i). E.g. (1<<0)|(1<<1) = timer + keyboard. */
void pic_init(uint16_t unmask);

/* Acknowledge IRQ `irq` (0-15) so the PIC will deliver the next one. */
void pic_eoi(int irq);

#endif /* EIGENOS_PIC_H */
