/* PS/2 keyboard (IRQ1) — the `read_char` HAL root. Scancode set 1 ->
 * ASCII, buffered in a ring so a reader can drain at its own pace. */
#ifndef EIGENOS_KEYBOARD_H
#define EIGENOS_KEYBOARD_H

void keyboard_init(void);   /* reset the ring + shift state */
void kbd_irq(void);         /* IRQ1 handler: read scancode, buffer ASCII */
int  kbd_getchar(void);     /* next char, or -1 if the buffer is empty */

#endif /* EIGENOS_KEYBOARD_H */
