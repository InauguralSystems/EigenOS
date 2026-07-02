/* VGA text console (milestone-N). */
#include "vga.h"
#include <stdint.h>

#define VGA   ((volatile uint16_t *)0xB8000)
#define COLS  80
#define ROWS  25
#define ATTR  0x0F                    /* white on black */

static int g_row, g_col;

static uint16_t cell(char c) { return (uint16_t)(unsigned char)c | ((uint16_t)ATTR << 8); }

void vga_clear(void) {
    for (int i = 0; i < COLS * ROWS; i++) VGA[i] = cell(' ');
    g_row = g_col = 0;
}

static void scroll(void) {
    for (int i = 0; i < COLS * (ROWS - 1); i++) VGA[i] = VGA[i + COLS];
    for (int i = COLS * (ROWS - 1); i < COLS * ROWS; i++) VGA[i] = cell(' ');
    g_row = ROWS - 1;
}

static void newline(void) {
    g_col = 0;
    if (++g_row >= ROWS) scroll();
}

void vga_putc(char c) {
    if (c == '\n') { newline(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\b') {
        if (g_col > 0) { g_col--; VGA[g_row * COLS + g_col] = cell(' '); }
        return;
    }
    if (c == '\t') { do { vga_putc(' '); } while (g_col % 4); return; }
    VGA[g_row * COLS + g_col] = cell(c);
    if (++g_col >= COLS) newline();
}

void vga_puts(const char *s) {
    for (; *s; s++) vga_putc(*s);
}
