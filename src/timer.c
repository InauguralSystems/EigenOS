/* PIT channel-0 tick source (milestone-4). */
#include "timer.h"
#include "io.h"

#define PIT_HZ   1193182u            /* base oscillator */
#define PIT_CH0  0x40
#define PIT_CMD  0x43

static volatile uint64_t g_ticks;
static uint32_t g_hz;

void timer_init(uint32_t hz) {
    if (hz == 0) hz = 100;
    uint32_t div = PIT_HZ / hz;
    if (div == 0) div = 1;
    if (div > 0xFFFF) div = 0xFFFF;
    g_hz = PIT_HZ / div;             /* the frequency we actually got */
    outb(PIT_CMD, 0x36);             /* ch0, lo/hi byte, mode 3 (square) */
    outb(PIT_CH0, (uint8_t)(div & 0xFF));
    outb(PIT_CH0, (uint8_t)((div >> 8) & 0xFF));
}

void     timer_irq(void)    { g_ticks++; }
uint64_t timer_ticks(void)  { return g_ticks; }
uint32_t timer_hz(void)     { return g_hz; }

/* M7: panic beep codes on the PC speaker (PIT channel 2 gated by port
 * 0x61). The one report channel that needs no video, no serial, and no
 * interrupts — usable from a cli'd exception handler. Durations use a
 * crude bounded spin (panic paths can't rely on IRQ ticks); accuracy is
 * irrelevant for a beep. Codes: 2 beeps = fatal CPU exception, 3 = stack
 * canary trampled. */
#define PIT_CH2  0x42
#define SPKR     0x61

static void spin(uint32_t n) {
    for (volatile uint32_t i = 0; i < n; i++) { }
}

void speaker_beeps(int count) {
    uint32_t div = PIT_HZ / 880;                 /* ~880 Hz */
    for (int b = 0; b < count; b++) {
        outb(PIT_CMD, 0xB6);                     /* ch2, lo/hi, mode 3 */
        outb(PIT_CH2, (uint8_t)(div & 0xFF));
        outb(PIT_CH2, (uint8_t)((div >> 8) & 0xFF));
        outb(SPKR, (uint8_t)(inb(SPKR) | 0x03)); /* gate + speaker on */
        spin(60u * 1000 * 1000);                 /* ~beep */
        outb(SPKR, (uint8_t)(inb(SPKR) & ~0x03u)); /* off */
        spin(30u * 1000 * 1000);                 /* ~gap */
    }
}
