/* VGA text console (milestone-N): a cursor-based, scrolling 80x25 text
 * screen — the output half of the on-metal REPL (the T3200's own display).
 * Distinct from kernel.c's fixed-row boot status; the REPL clears the
 * screen and drives this console. */
#ifndef EIGENOS_VGA_H
#define EIGENOS_VGA_H

#include <stdint.h>

void vga_clear(void);
void vga_putc(char c);          /* handles '\n', '\b', wrap, and scroll */
void vga_puts(const char *s);
void vga_put_at(int row, int col, const char *s, uint8_t attr);  /* fixed row */

#endif /* EIGENOS_VGA_H */
