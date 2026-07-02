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
