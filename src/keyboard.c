/* PS/2 keyboard driver (milestone-4).
 *
 * QEMU (and a real 8042 after BIOS setup) delivers scancode set 1 by
 * default, so no controller reprogramming is needed here — just read
 * port 0x60 on IRQ1, translate, and buffer. US layout, letters honor a
 * tracked shift state; other keys use the base table. */
#include "keyboard.h"
#include "io.h"
#include <stdint.h>

#define KBD_DATA 0x60
#define RING_SZ  256             /* power of two */

static char     g_ring[RING_SZ];
static volatile unsigned g_head, g_tail;
static int      g_shift;

static const char kmap[128] = {
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

void keyboard_init(void) {
    g_head = g_tail = 0;
    g_shift = 0;
}

static void ring_push(char c) {
    unsigned next = (g_head + 1) & (RING_SZ - 1);
    if (next == g_tail) return;      /* full: drop (never blocks the IRQ) */
    g_ring[g_head] = c;
    g_head = next;
}

void kbd_irq(void) {
    uint8_t sc = inb(KBD_DATA);
    if (sc == 0x2A || sc == 0x36) { g_shift = 1; return; }   /* shift down */
    if (sc == 0xAA || sc == 0xB6) { g_shift = 0; return; }   /* shift up   */
    if (sc & 0x80) return;                                    /* other release */
    char c = kmap[sc & 0x7F];
    if (!c) return;
    if (g_shift && c >= 'a' && c <= 'z') c = (char)(c - 32);
    ring_push(c);
}

int kbd_getchar(void) {
    if (g_tail == g_head) return -1;
    char c = g_ring[g_tail];
    g_tail = (g_tail + 1) & (RING_SZ - 1);
    return (int)(unsigned char)c;
}
