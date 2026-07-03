/* Linear-framebuffer backend (milestone-9.5): GRUB/VBE hands over a
 * direct-RGB LFB via the Multiboot2 framebuffer tag; this module owns
 * the pixels. Two clients:
 *
 *   - the console (vga.c): font-blit text cells, rendered immediately
 *     (shadow + VRAM) so boot verdicts and the REPL stay live;
 *   - the EigenScript gfx builtins (hal.c): draw into the RAM shadow
 *     only, fb_present() copies to VRAM — hosted double-buffer
 *     semantics, and VRAM is never read (UC reads are glacial on real
 *     hardware).
 *
 * Built with the KERNEL cflags (no SSE): the panic path renders from
 * IRQ context. Bulk copies are rep-string inline asm so -O2 can never
 * turn them into calls to the SSE-compiled mini-libc mem*. */
#ifndef EIGENOS_FB_H
#define EIGENOS_FB_H

#include <stdint.h>

/* MB2 parse-time handoff (before paging; no access yet). */
void fb_set_info(uint64_t phys, uint32_t pitch, uint32_t w, uint32_t h,
                 uint8_t bpp, uint8_t rpos, uint8_t gpos, uint8_t bpos);
int  fb_present_mode(void);      /* MB2 promised a usable RGB fb (32 bpp) */

/* Map the LFB + allocate the shadow. Needs paging + kheap live.
 * Returns 0 on success; on failure the console stays on VGA text. */
int  fb_activate(void);
int  fb_active(void);
uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_bpp(void);
uint64_t fb_phys(void);

/* Console cells (immediate: shadow + VRAM). 5x7 font at scale 2 in a
 * FB_CELL_W x FB_CELL_H box; attr is a VGA attribute byte (fg|bg<<4). */
#define FB_CELL_W 12
#define FB_CELL_H 16
void fb_draw_cell(int row, int col, char ch, uint8_t attr);
void fb_batch(int on);           /* on: draw_cell skips its VRAM strip
                                    (bulk repaint; fb_present() flushes) */
void fb_present(void);           /* whole shadow -> VRAM */

/* Gfx primitives (shadow only; fb_present shows the frame).
 * Coordinates clip; rgb are 0..255; a=255 is opaque, else src-over.
 * fb_gfx_clip bounds every primitive to a rectangle — the MD window
 * viewport seam: a task's drawing cannot escape its window. */
void fb_gfx_clip(int x, int y, int w, int h);
void fb_gfx_clip_reset(void);
void fb_gfx_clear(int r, int g, int b);
void fb_gfx_fill(int x, int y, int w, int h, int r, int g, int b, int a);
void fb_gfx_point(int x, int y, int r, int g, int b);
void fb_gfx_line(int x0, int y0, int x1, int y1, int r, int g, int b);
void fb_gfx_circle(int cx, int cy, int rad, int r, int g, int b);
void fb_gfx_text(int x, int y, const char *s, int r, int g, int b, int scale);

/* Self-test probe: read ONE pixel back from VRAM (slow, test-only). */
uint32_t fb_vram_pixel(int x, int y);
uint32_t fb_pack(int r, int g, int b);

#endif /* EIGENOS_FB_H */
