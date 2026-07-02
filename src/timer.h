/* PIT (8253/8254) channel-0 tick source — the `clock` HAL root: a
 * monotonic tick counter driven by IRQ0. */
#ifndef EIGENOS_TIMER_H
#define EIGENOS_TIMER_H

#include <stdint.h>

void     timer_init(uint32_t hz);   /* program channel 0 at `hz` and record it */
void     timer_irq(void);           /* IRQ0 handler: bump the tick count */
uint64_t timer_ticks(void);         /* ticks since init */
uint32_t timer_hz(void);            /* the configured frequency */
void     speaker_beeps(int count);  /* panic beep codes (PIT ch2, no IRQs) */

#endif /* EIGENOS_TIMER_H */
