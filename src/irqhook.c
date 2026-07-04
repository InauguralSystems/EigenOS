/* M12 phase 2: per-IRQ claim flags + raw-byte rings (see irqhook.h).
 * Kernel-flags file: irq_hook_push runs in IRQ context (same volatile
 * head/tail discipline as the keyboard/mouse rings — IRQ writes, evals
 * read). Only the byte-producing 8042 pair (IRQ1 keyboard, IRQ12 aux)
 * is claimable; the refusal is loud at the builtin layer. */
#include "irqhook.h"

#define RING_SZ 256                /* power of two, matches keyboard.c */
#define NIRQ    16

static volatile uint8_t g_claimed[NIRQ];
static uint8_t          g_ring[NIRQ][RING_SZ];
static volatile unsigned g_head[NIRQ], g_tail[NIRQ];
static volatile unsigned g_drop[NIRQ];

static int supported(int irq) { return irq == 1 || irq == 12; }

int irq_hook_claim(int irq) {
    if (irq < 0 || irq >= NIRQ || !supported(irq)) return 0;
    g_head[irq] = g_tail[irq] = 0;     /* no stale bytes from a past claim */
    g_drop[irq] = 0;
    g_claimed[irq] = 1;
    return 1;
}

void irq_hook_release(int irq) {
    if (irq < 0 || irq >= NIRQ) return;
    g_claimed[irq] = 0;
}

int irq_hook_release_all(void) {
    int n = 0;
    for (int i = 0; i < NIRQ; i++) {
        if (g_claimed[i]) { g_claimed[i] = 0; n++; }
    }
    return n;
}

int irq_hook_claimed(int irq) {
    return (irq >= 0 && irq < NIRQ) ? g_claimed[irq] : 0;
}

void irq_hook_push(int irq, uint8_t b) {
    unsigned next = (g_head[irq] + 1) & (RING_SZ - 1);
    if (next == g_tail[irq]) { g_drop[irq]++; return; }   /* full: drop */
    g_ring[irq][g_head[irq]] = b;
    g_head[irq] = next;
}

int irq_hook_read(int irq) {
    if (irq < 0 || irq >= NIRQ) return -1;
    if (g_tail[irq] == g_head[irq]) return -1;
    uint8_t b = g_ring[irq][g_tail[irq]];
    g_tail[irq] = (g_tail[irq] + 1) & (RING_SZ - 1);
    return (int)b;
}

int irq_hook_dropped(int irq) {
    return (irq >= 0 && irq < NIRQ) ? (int)g_drop[irq] : 0;
}
