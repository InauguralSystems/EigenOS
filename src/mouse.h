/* PS/2 mouse driver (milestone-9.5) — the pointer half of "pixels and a
 * pointer". IRQ12 via the i8042 aux port; the T3200 touchpad is PS/2
 * aux behind the same controller as the keyboard. */
#ifndef EIGENOS_MOUSE_H
#define EIGENOS_MOUSE_H

#include <stdint.h>

/* Enable the aux device + IRQ12 streaming. Bounded waits throughout —
 * a machine with no aux port FAILS the self-test, never hangs boot.
 * Returns 1 if the mouse ACKed enable-streaming. */
int  mouse_init(void);
int  mouse_present(void);

void mouse_irq(void);                    /* IRQ12: consume aux bytes */
void mouse_byte(uint8_t b);              /* stray aux byte seen by kbd_irq */

/* Movement applies to the tracked position at IRQ time; a packet ring
 * records each step for event-style consumers (gfx_poll). */
void mouse_set_bounds(int w, int h);
int  mouse_x(void);
int  mouse_y(void);
int  mouse_buttons(void);                /* bit0=left bit1=right bit2=middle */

/* Pop one packet: post-move position + buttons + whether it moved.
 * Returns 0 when the ring is empty. */
int  mouse_next_packet(int *x, int *y, int *buttons, int *moved);

#endif /* EIGENOS_MOUSE_H */
