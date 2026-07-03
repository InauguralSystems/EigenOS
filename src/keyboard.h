/* PS/2 keyboard (IRQ1) — the `read_char` HAL root, extended for MD's
 * input dispatch (M9.5 debts): E0 extended scancodes (arrows/home/end/
 * delete), modifier tracking, and a key-EVENT stream (down AND up) for
 * gfx_poll beside the char stream the REPL drains.
 *
 * Two consumers, one mode flag: at the REPL (gfx mode off) translated
 * chars land in the char ring; while a gfx app owns the screen (gfx
 * mode on) key events land in the event ring instead — so a quit key
 * doesn't replay into the REPL afterward, and releases exist for
 * key-held game/UI state. */
#ifndef EIGENOS_KEYBOARD_H
#define EIGENOS_KEYBOARD_H

/* Non-ASCII keys, encoded above the ASCII range in both streams. */
#define KEY_UP     0x80
#define KEY_DOWN   0x81
#define KEY_LEFT   0x82
#define KEY_RIGHT  0x83
#define KEY_HOME   0x84
#define KEY_END    0x85
#define KEY_DELETE 0x86
#define KEY_PGUP   0x87
#define KEY_PGDN   0x88

#define KMOD_SHIFT 1
#define KMOD_CTRL  2
#define KMOD_ALT   4

void keyboard_init(void);   /* reset rings + modifier state */
void kbd_irq(void);         /* IRQ1 handler: read scancode, buffer */
int  kbd_getchar(void);     /* next char/KEY_* code, or -1 (REPL stream) */

void kbd_gfx_mode(int on);  /* route input to the event stream (+flush) */
int  kbd_next_event(int *code, int *down, int *mods);  /* 0 = empty */
int  kbd_mods(void);        /* live modifier state (KMOD_*) */

#endif /* EIGENOS_KEYBOARD_H */
