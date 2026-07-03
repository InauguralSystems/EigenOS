/* Text console (milestone-N, generalized in M9.5): a cursor-based,
 * scrolling cell grid — the output half of the on-metal REPL. The grid
 * is the source of truth; the renderer behind it is either VGA text at
 * 0xB8000 (the M0 path, still the fallback) or the M9.5 framebuffer
 * console (font-blit via fb.c). Boot output before the framebuffer can
 * be mapped accumulates in the grid and repaints on activation.
 * Distinct from kernel.c's fixed-row boot status; the REPL clears the
 * screen and drives this console. */
#ifndef EIGENOS_VGA_H
#define EIGENOS_VGA_H

#include <stdint.h>

void vga_clear(void);
void vga_putc(char c);          /* handles '\n', '\b', wrap, and scroll */
void vga_puts(const char *s);
void vga_put_at(int row, int col, const char *s, uint8_t attr);  /* fixed row */

int  vga_rows(void);            /* active grid height (25 text / 48 fb) */
int  vga_cols(void);

/* M9.5 mode plumbing (kernel_main drives these):
 *  - vga_fb_pending(): MB2 promised an RGB framebuffer — there is no
 *    text memory to write; buffer to the grid until activation.
 *  - vga_fb_activate(): map the LFB (fb_activate) and repaint the grid
 *    as font-blit cells. Returns 0 on success; on failure the console
 *    reverts to VGA text (and replays the grid there). */
void vga_fb_pending(void);
int  vga_fb_activate(void);

/* Gfx handoff: while suspended the grid still records every write (and
 * serial still logs), but nothing renders — the EigenScript gfx
 * builtins own the pixels. Resume repaints the grid. */
void vga_suspend(int on);
int  vga_suspended(void);
void vga_repaint(void);

#endif /* EIGENOS_VGA_H */
