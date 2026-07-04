/* PS/2 keyboard driver (milestone-4; events + E0 keys for MD groundwork).
 *
 * QEMU (and a real 8042 after BIOS setup) delivers scancode set 1 by
 * default, so no controller reprogramming is needed here — just read
 * port 0x60 on IRQ1, translate, and buffer. US layout; tracked
 * shift/ctrl/alt state; 0xE0-prefixed extended keys (arrows, home/end,
 * delete, pgup/pgdn) map to KEY_* codes above the ASCII range. */
#include "keyboard.h"
#include "mouse.h"
#include "irqhook.h"
#include "io.h"
#include <stdint.h>

#define KBD_DATA   0x60
#define KBD_STATUS 0x64
#define ST_AUX     0x20          /* pending byte belongs to the aux port */
#define RING_SZ    256           /* power of two */

/* Char stream (REPL). */
static char     g_ring[RING_SZ];
static volatile unsigned g_head, g_tail;

/* Event stream (gfx_poll): code:8 | down:8 | mods:8, down+up both. */
static uint32_t g_ering[RING_SZ];
static volatile unsigned g_ehead, g_etail;

static int g_mods;               /* KMOD_* bitmask */
static int g_e0;                 /* saw an 0xE0 prefix byte */
static int g_gfx;                /* events mode (gfx app owns input) */

static const char kmap[128] = {
    [0x01]=27,                   /* ESC */
    [0x02]='1',[0x03]='2',[0x04]='3',[0x05]='4',[0x06]='5',
    [0x07]='6',[0x08]='7',[0x09]='8',[0x0A]='9',[0x0B]='0',
    [0x0C]='-',[0x0D]='=',[0x0E]='\b',[0x0F]='\t',
    [0x10]='q',[0x11]='w',[0x12]='e',[0x13]='r',[0x14]='t',[0x15]='y',
    [0x16]='u',[0x17]='i',[0x18]='o',[0x19]='p',[0x1A]='[',[0x1B]=']',
    [0x1C]='\n',
    [0x1E]='a',[0x1F]='s',[0x20]='d',[0x21]='f',[0x22]='g',[0x23]='h',
    [0x24]='j',[0x25]='k',[0x26]='l',[0x27]=';',[0x28]='\'',[0x29]='`',
    [0x2B]='\\',
    [0x2C]='z',[0x2D]='x',[0x2E]='c',[0x2F]='v',[0x30]='b',[0x31]='n',
    [0x32]='m',[0x33]=',',[0x34]='.',[0x35]='/',
    [0x39]=' ',
};

/* M6: shifted non-letter keys (US layout). Letters keep the arithmetic
 * uppercase path in kbd_irq; everything else EigenScript needs from a
 * keyboard — * + " ( ) [ ] { } : < > ? ! — lives here. */
static const char kmap_shift[128] = {
    [0x02]='!',[0x03]='@',[0x04]='#',[0x05]='$',[0x06]='%',
    [0x07]='^',[0x08]='&',[0x09]='*',[0x0A]='(',[0x0B]=')',
    [0x0C]='_',[0x0D]='+',
    [0x1A]='{',[0x1B]='}',
    [0x27]=':',[0x28]='"',[0x29]='~',
    [0x2B]='|',
    [0x33]='<',[0x34]='>',[0x35]='?',
};

/* 0xE0-prefixed keys (set 1) -> KEY_* codes. */
static const unsigned char kmap_e0[128] = {
    [0x48]=KEY_UP,  [0x50]=KEY_DOWN, [0x4B]=KEY_LEFT, [0x4D]=KEY_RIGHT,
    [0x47]=KEY_HOME,[0x4F]=KEY_END,  [0x53]=KEY_DELETE,
    [0x49]=KEY_PGUP,[0x51]=KEY_PGDN,
    [0x1C]='\n',                 /* keypad enter */
    [0x35]='/',                  /* keypad slash */
};

void keyboard_init(void) {
    g_head = g_tail = g_ehead = g_etail = 0;
    g_mods = g_e0 = g_gfx = 0;
}

void kbd_gfx_mode(int on) {
    g_gfx = on ? 1 : 0;
    g_head = g_tail = 0;         /* no stale REPL chars after a gfx app */
    g_ehead = g_etail = 0;       /* and no stale events before one */
}

int kbd_mods(void) { return g_mods; }

static void ring_push(char c) {
    unsigned next = (g_head + 1) & (RING_SZ - 1);
    if (next == g_tail) return;      /* full: drop (never blocks the IRQ) */
    g_ring[g_head] = c;
    g_head = next;
}

static void ering_push_m(int code, int down, int mods) {
    unsigned next = (g_ehead + 1) & (RING_SZ - 1);
    if (next == g_etail) return;
    g_ering[g_ehead] = (uint32_t)(code & 0xFF) |
                       ((uint32_t)(down & 1) << 8) |
                       ((uint32_t)(mods & 0xFF) << 16);
    g_ehead = next;
}
static void ering_push(int code, int down) { ering_push_m(code, down, g_mods); }

/* M12 phase 3: the script driver's push-back (see keyboard.h). The
 * driver's modifier snapshot also becomes the LIVE kbd_mods() reading —
 * with IRQ1 claimed the C translator never sees a shift/ctrl byte, and
 * the line editor's ctrl-key handling reads kbd_mods() per char. */
void kbd_inject(int code, int down, int mods) {
    g_mods = mods & 0xFF;
    if (code <= 0) return;
    if (g_gfx) ering_push_m(code, down, mods);
    else if (down) ring_push((char)code);
}

void kbd_irq(void) {
    /* One 8042 serves both devices: if the pending byte is aux-flagged
     * it is mouse traffic that raced onto IRQ1 — route it (M9.5). */
    if (inb(KBD_STATUS) & ST_AUX) {
        uint8_t ab = inb(KBD_DATA);
        if (irq_hook_claimed(12)) irq_hook_push(12, ab);
        else mouse_byte(ab);
        return;
    }
    uint8_t sc = inb(KBD_DATA);

    /* M12: a claim diverts the RAW stream (E0 prefixes and all) before
     * any translation — the .eigs driver owns the state machine. */
    if (irq_hook_claimed(1)) { irq_hook_push(1, sc); return; }

    if (sc == 0xE0) { g_e0 = 1; return; }
    int e0 = g_e0;
    g_e0 = 0;

    int idx  = sc & 0x7F;
    int down = !(sc & 0x80);

    /* Modifiers (E0 0x1D/0x38 are right-ctrl/alt-gr — same bits). The
     * E0 2A/AA "fake shift" bytes some sets emit around extended keys
     * must NOT flip real shift state. */
    if (idx == 0x2A || idx == 0x36) {
        if (!e0) g_mods = down ? (g_mods | KMOD_SHIFT) : (g_mods & ~KMOD_SHIFT);
        return;
    }
    if (idx == 0x1D) { g_mods = down ? (g_mods | KMOD_CTRL) : (g_mods & ~KMOD_CTRL); return; }
    if (idx == 0x38) { g_mods = down ? (g_mods | KMOD_ALT)  : (g_mods & ~KMOD_ALT);  return; }

    int c;
    if (e0) {
        c = kmap_e0[idx];
    } else if ((g_mods & KMOD_SHIFT) && kmap_shift[idx]) {
        c = kmap_shift[idx];                          /* shifted symbol */
    } else {
        c = kmap[idx];
        if ((g_mods & KMOD_SHIFT) && c >= 'a' && c <= 'z') c -= 32;
    }
    if (!c) return;

    if (g_gfx) ering_push(c, down);                   /* down AND up */
    else if (down) ring_push((char)c);
}

int kbd_getchar(void) {
    if (g_tail == g_head) return -1;
    char c = g_ring[g_tail];
    g_tail = (g_tail + 1) & (RING_SZ - 1);
    return (int)(unsigned char)c;
}

int kbd_next_event(int *code, int *down, int *mods) {
    if (g_etail == g_ehead) return 0;
    uint32_t v = g_ering[g_etail];
    g_etail = (g_etail + 1) & (RING_SZ - 1);
    *code = (int)(v & 0xFF);
    *down = (int)((v >> 8) & 1);
    *mods = (int)((v >> 16) & 0xFF);
    return 1;
}
