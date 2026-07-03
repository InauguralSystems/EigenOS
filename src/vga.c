/* Text console over a cell grid (milestone-N; fb backend in M9.5). */
#include "vga.h"
#include "fb.h"
#include <stdint.h>

#define VGA        ((volatile uint16_t *)0xB8000)
#define TEXT_COLS  80
#define TEXT_ROWS  25
#define MAX_COLS   128
#define MAX_ROWS   64
#define ATTR       0x0F                   /* white on black */

/* The grid: what the screen WOULD show. Rendered by the active backend;
 * replayed whole on backend switches and gfx resume. */
static char    g_chs[MAX_ROWS][MAX_COLS];
static uint8_t g_ats[MAX_ROWS][MAX_COLS];
static int g_cols = TEXT_COLS, g_rows = TEXT_ROWS;
static int g_row, g_col;

enum { MODE_TEXT, MODE_FB_PENDING, MODE_FB };
static int g_mode = MODE_TEXT;
static int g_suspend;                     /* gfx owns the pixels */

int  vga_rows(void)      { return g_rows; }
int  vga_cols(void)      { return g_cols; }
int  vga_suspended(void) { return g_suspend; }

static void render_cell(int row, int col) {
    if (g_suspend) return;
    if (g_mode == MODE_FB)
        fb_draw_cell(row, col, g_chs[row][col], g_ats[row][col]);
    else if (g_mode == MODE_TEXT && row < TEXT_ROWS && col < TEXT_COLS)
        VGA[row * TEXT_COLS + col] =
            (uint16_t)(unsigned char)g_chs[row][col] |
            ((uint16_t)g_ats[row][col] << 8);
}

static void set_cell(int row, int col, char c, uint8_t attr) {
    g_chs[row][col] = c;
    g_ats[row][col] = attr;
    render_cell(row, col);
}

void vga_repaint(void) {
    int flush = (g_mode == MODE_FB && !g_suspend);
    if (flush) fb_batch(1);
    for (int r = 0; r < g_rows; r++)
        for (int c = 0; c < g_cols; c++)
            render_cell(r, c);
    if (flush) { fb_batch(0); fb_present(); }
}

void vga_suspend(int on) {
    if (g_suspend && !on) { g_suspend = 0; vga_repaint(); return; }
    g_suspend = on ? 1 : 0;
}

void vga_fb_pending(void) { g_mode = MODE_FB_PENDING; }

int vga_fb_activate(void) {
    if (fb_activate() == 0) {
        g_mode = MODE_FB;
        g_cols = (int)(fb_width() / FB_CELL_W);
        g_rows = (int)(fb_height() / FB_CELL_H);
        if (g_cols > MAX_COLS) g_cols = MAX_COLS;
        if (g_rows > MAX_ROWS) g_rows = MAX_ROWS;
        vga_repaint();
        return 0;
    }
    g_mode = MODE_TEXT;                   /* VBE reneged: replay on text */
    g_cols = TEXT_COLS; g_rows = TEXT_ROWS;
    vga_repaint();
    return -1;
}

void vga_clear(void) {
    for (int r = 0; r < MAX_ROWS; r++)
        for (int c = 0; c < MAX_COLS; c++) {
            g_chs[r][c] = ' ';
            g_ats[r][c] = ATTR;
        }
    g_row = g_col = 0;
    vga_repaint();
}

static void scroll(void) {
    for (int r = 0; r < g_rows - 1; r++)
        for (int c = 0; c < g_cols; c++) {
            g_chs[r][c] = g_chs[r + 1][c];
            g_ats[r][c] = g_ats[r + 1][c];
        }
    for (int c = 0; c < g_cols; c++) {
        g_chs[g_rows - 1][c] = ' ';
        g_ats[g_rows - 1][c] = ATTR;
    }
    g_row = g_rows - 1;
    vga_repaint();                        /* every row moved; one bulk present */
}

static void newline(void) {
    g_col = 0;
    if (++g_row >= g_rows) scroll();
}

void vga_putc(char c) {
    if (c == '\n') { newline(); return; }
    if (c == '\r') { g_col = 0; return; }
    if (c == '\b') {
        if (g_col > 0) { g_col--; set_cell(g_row, g_col, ' ', ATTR); }
        return;
    }
    if (c == '\t') { do { vga_putc(' '); } while (g_col % 4); return; }
    set_cell(g_row, g_col, c, ATTR);
    if (++g_col >= g_cols) newline();
}

void vga_puts(const char *s) {
    for (; *s; s++) vga_putc(*s);
}

/* Fixed-row write that bypasses the scrolling cursor — for boot verdicts
 * that must be VISIBLE on hardware with no serial (M7: the demo verdicts
 * join kernel.c's status board rows instead of vanishing into COM1). */
void vga_put_at(int row, int col, const char *s, uint8_t attr) {
    if (row < 0 || row >= g_rows || col < 0) return;
    for (; *s && col < g_cols; s++, col++)
        set_cell(row, col, *s, attr);
}
