/* PS/2 mouse driver (milestone-9.5). Standard 3-byte packets (no wheel
 * negotiation — the base protocol every aux device speaks, including
 * the T3200 touchpad): byte0 = flags/buttons with bit3 always set (the
 * resync anchor), byte1 = dx, byte2 = dy, 9-bit two's complement, PS/2
 * y-up flipped to screen y-down here. */
#include "mouse.h"
#include "io.h"

#define PS2_DATA   0x60
#define PS2_STATUS 0x64                  /* read: status; write: command */
#define PS2_CMD    0x64

#define ST_OBF  0x01                     /* output buffer full (read ok) */
#define ST_IBF  0x02                     /* input buffer full (don't write) */
#define ST_AUX  0x20                     /* pending byte is from the aux port */

#define CMD_READ_CFG   0x20
#define CMD_WRITE_CFG  0x60
#define CMD_AUX_ENABLE 0xA8
#define CMD_AUX_SEND   0xD4
#define MOUSE_DEFAULTS 0xF6
#define MOUSE_STREAM   0xF4
#define MOUSE_ACK      0xFA

#define WAIT_SPINS 100000                /* bounded: a dead port fails fast */

static int g_present;
static int g_x, g_y, g_btn;
static int g_w = 1, g_h = 1;

/* Packet assembly + ring. Entries pack post-move x(12) y(12) btn(3)
 * moved(1) into a uint32 — IRQ writes, poll reads, same discipline as
 * the keyboard ring (full = drop, never block the IRQ). */
#define RING_SZ 64                       /* power of two */
static uint32_t g_ring[RING_SZ];
static volatile unsigned g_head, g_tail;
static uint8_t g_pkt[3];
static int     g_phase;

static int wait_write(void) {
    for (int i = 0; i < WAIT_SPINS; i++)
        if (!(inb(PS2_STATUS) & ST_IBF)) return 1;
    return 0;
}
static int wait_read(void) {
    for (int i = 0; i < WAIT_SPINS; i++)
        if (inb(PS2_STATUS) & ST_OBF) return 1;
    return 0;
}

static int aux_command(uint8_t cmd) {    /* send to the MOUSE, expect ACK */
    if (!wait_write()) return 0;
    outb(PS2_CMD, CMD_AUX_SEND);
    if (!wait_write()) return 0;
    outb(PS2_DATA, cmd);
    if (!wait_read()) return 0;
    return inb(PS2_DATA) == MOUSE_ACK;
}

int mouse_init(void) {
    /* Enable the aux port, then set config: aux IRQ on (bit1), aux
     * clock running (bit5 clear). Keyboard bits untouched. */
    if (!wait_write()) return 0;
    outb(PS2_CMD, CMD_AUX_ENABLE);
    if (!wait_write()) return 0;
    outb(PS2_CMD, CMD_READ_CFG);
    if (!wait_read()) return 0;
    uint8_t cfg = inb(PS2_DATA);
    cfg |= 0x02;                         /* aux interrupt enable */
    cfg &= (uint8_t)~0x20;               /* aux clock enable */
    if (!wait_write()) return 0;
    outb(PS2_CMD, CMD_WRITE_CFG);
    if (!wait_write()) return 0;
    outb(PS2_DATA, cfg);

    if (!aux_command(MOUSE_DEFAULTS)) return 0;
    if (!aux_command(MOUSE_STREAM))  return 0;
    g_present = 1;
    return 1;
}

int mouse_present(void) { return g_present; }

void mouse_set_bounds(int w, int h) {
    if (w > 0) g_w = w;
    if (h > 0) g_h = h;
    g_x = g_w / 2;
    g_y = g_h / 2;
}

int mouse_x(void)       { return g_x; }
int mouse_y(void)       { return g_y; }
int mouse_buttons(void) { return g_btn; }

static void ring_push(uint32_t v) {
    unsigned next = (g_head + 1) & (RING_SZ - 1);
    if (next == g_tail) return;          /* full: drop */
    g_ring[g_head] = v;
    g_head = next;
}

static void packet_done(void) {
    uint8_t f = g_pkt[0];
    if (f & 0xC0) return;                /* x/y overflow: discard */
    int dx = (int)g_pkt[1] - (int)((f << 4) & 0x100);
    int dy = (int)g_pkt[2] - (int)((f << 3) & 0x100);
    g_btn = f & 0x07;
    g_x += dx;
    g_y -= dy;                           /* PS/2 y+ is up; screen y+ is down */
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x >= g_w) g_x = g_w - 1;
    if (g_y >= g_h) g_y = g_h - 1;
    ring_push(((uint32_t)(g_x & 0xFFF) << 20) |
              ((uint32_t)(g_y & 0xFFF) << 8)  |
              ((uint32_t)(g_btn & 0x7) << 1)  |
              (uint32_t)((dx | dy) != 0));
}

void mouse_byte(uint8_t b) {
    if (g_phase == 0 && !(b & 0x08)) return;   /* resync on the anchor bit */
    g_pkt[g_phase++] = b;
    if (g_phase == 3) {
        g_phase = 0;
        packet_done();
    }
}

void mouse_irq(void) {
    /* Drain everything aux-flagged; leave keyboard bytes for IRQ1. */
    while ((inb(PS2_STATUS) & (ST_OBF | ST_AUX)) == (ST_OBF | ST_AUX))
        mouse_byte(inb(PS2_DATA));
}

int mouse_next_packet(int *x, int *y, int *buttons, int *moved) {
    if (g_tail == g_head) return 0;
    uint32_t v = g_ring[g_tail];
    g_tail = (g_tail + 1) & (RING_SZ - 1);
    *x = (int)((v >> 20) & 0xFFF);
    *y = (int)((v >> 8) & 0xFFF);
    *buttons = (int)((v >> 1) & 0x7);
    *moved = (int)(v & 1);
    return 1;
}
