/* M12 phase 2: the IRQ-claim seam — raw device bytes for .eigs drivers.
 *
 * A claimed IRQ's bytes divert into a per-IRQ ring BEFORE the C
 * driver's translation (for IRQ1 that means raw scancodes, 0xE0
 * prefixes and all); the language drains the ring cooperatively via
 * irq_read between evals. IRQ context stays pure C — the "callback"
 * is the drain, never a runtime call. Releasing a claim (explicitly,
 * or the REPL's eval-error recovery) falls back to the built-in C
 * driver, so input can never die from a script error. This sits BELOW
 * keyboard.c's REPL/gfx mode flag: a claimed byte reaches neither
 * translated ring. */
#ifndef EIGENOS_IRQHOOK_H
#define EIGENOS_IRQHOOK_H

#include <stdint.h>

int  irq_hook_claim(int irq);      /* 1 ok; 0 refused (only IRQ 1/12 divert) */
void irq_hook_release(int irq);
int  irq_hook_release_all(void);   /* -> how many claims were dropped */
int  irq_hook_claimed(int irq);    /* consulted on the IRQ path */
void irq_hook_push(int irq, uint8_t b);  /* IRQ context: full = drop, never block */
int  irq_hook_read(int irq);       /* next raw byte, -1 if empty/unclaimed */
int  irq_hook_pending(int irq);    /* ring non-empty? (cheap peek, no pop) */
int  irq_hook_dropped(int irq);    /* bytes lost to a full ring (live gauge) */

#endif /* EIGENOS_IRQHOOK_H */
