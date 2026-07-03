/* Linear-framebuffer backend (milestone-9.5). See fb.h for the model. */
#include "fb.h"
#include "paging.h"
#include "kheap.h"
#include "font5x7.h"          /* generated: tools/mkfont.py from ext_gfx.c */

/* The LFB maps into the on-demand window, clear of the paging self-test
 * page at KVMAP_BASE and far below the kheap's low-canonical arena. */
#define FB_VIRT 0xFFFFC00080000000ULL

static uint64_t g_phys;
static uint32_t g_pitch, g_w, g_h;
static uint8_t  g_bpp, g_rpos, g_gpos, g_bpos;
static int      g_usable;                 /* parse said RGB + 32 bpp */
static int      g_active;
static int      g_batch;
static volatile uint8_t *g_vram;
static uint8_t *g_shadow;

/* rep-string bulk ops: immune to -O2 loop-idiom recognition emitting
 * calls into the SSE mini-libc (an IRQ-path XMM hazard), and fast on
 * everything back to the Core 2. cld: don't trust DF in a panic path. */
static void fb_copy8(void *dst, const void *src, uint64_t qwords) {
    __asm__ volatile ("cld; rep movsq"
                      : "+D"(dst), "+S"(src), "+c"(qwords) :: "memory");
}
static void fb_fill8(void *dst, uint64_t pattern, uint64_t qwords) {
    __asm__ volatile ("cld; rep stosq"
                      : "+D"(dst), "+c"(qwords) : "a"(pattern) : "memory");
}

void fb_set_info(uint64_t phys, uint32_t pitch, uint32_t w, uint32_t h,
                 uint8_t bpp, uint8_t rpos, uint8_t gpos, uint8_t bpos) {
    g_phys = phys; g_pitch = pitch; g_w = w; g_h = h;
    g_bpp = bpp; g_rpos = rpos; g_gpos = gpos; g_bpos = bpos;
    /* 32 bpp only for now: one pixel = one aligned uint32 store. If real
     * hardware ever answers the 1024x768x32 request with 24 bpp, the
     * kernel boots on the text console and says so — add the 3-byte
     * path then, against the machine that wants it. */
    g_usable = (w > 0 && h > 0 && bpp == 32 && pitch >= w * 4);
}

int fb_present_mode(void) { return g_usable; }
int fb_active(void)       { return g_active; }
uint32_t fb_width(void)   { return g_w; }
uint32_t fb_height(void)  { return g_h; }
uint32_t fb_bpp(void)     { return g_bpp; }
uint64_t fb_phys(void)    { return g_phys; }

uint32_t fb_pack(int r, int g, int b) {
    return ((uint32_t)(r & 0xFF) << g_rpos) |
           ((uint32_t)(g & 0xFF) << g_gpos) |
           ((uint32_t)(b & 0xFF) << g_bpos);
}

int fb_activate(void) {
    if (!g_usable || g_active) return g_active ? 0 : -1;
    uint64_t bytes = (uint64_t)g_pitch * g_h;
    uint64_t pages = (bytes + 0xFFF) >> 12;
    for (uint64_t i = 0; i < pages; i++)
        if (map_page(FB_VIRT + (i << 12), g_phys + (i << 12), PG_WRITE) != 0)
            return -1;
    g_shadow = kmalloc(bytes);            /* kheap zero-fills: black frame */
    if (!g_shadow) return -1;
    g_vram = (volatile uint8_t *)FB_VIRT;
    g_active = 1;
    fb_gfx_clip_reset();
    fb_present();
    return 0;
}

void fb_present(void) {
    if (!g_active) return;
    fb_copy8((void *)g_vram, g_shadow, ((uint64_t)g_pitch * g_h) >> 3);
}

/* ---- console cells ---- */

/* VGA attribute nibble -> RGB, the classic 16-color palette, so the
 * boot board's 0x0A green / 0x4F white-on-red verdicts carry over. */
static const uint32_t vga_rgb[16][3] = {
    {0,0,0},       {0,0,170},    {0,170,0},    {0,170,170},
    {170,0,0},     {170,0,170},  {170,85,0},   {170,170,170},
    {85,85,85},    {85,85,255},  {85,255,85},  {85,255,255},
    {255,85,85},   {255,85,255}, {255,255,85}, {255,255,255},
};

static uint32_t attr_fg(uint8_t attr) {
    const uint32_t *c = vga_rgb[attr & 0xF];
    return fb_pack((int)c[0], (int)c[1], (int)c[2]);
}
static uint32_t attr_bg(uint8_t attr) {
    const uint32_t *c = vga_rgb[(attr >> 4) & 0x7];
    return fb_pack((int)c[0], (int)c[1], (int)c[2]);
}

static inline uint32_t *shadow_px(int x, int y) {
    return (uint32_t *)(g_shadow + (uint64_t)y * g_pitch + (uint64_t)x * 4);
}

void fb_draw_cell(int row, int col, char ch, uint8_t attr) {
    if (!g_active) return;
    int x0 = col * FB_CELL_W, y0 = row * FB_CELL_H;
    if (x0 < 0 || y0 < 0 ||
        x0 + FB_CELL_W > (int)g_w || y0 + FB_CELL_H > (int)g_h) return;

    uint32_t fg = attr_fg(attr), bg = attr_bg(attr);
    int c = (unsigned char)ch;
    const unsigned char *glyph =
        (c >= 32 && c <= 126) ? font5x7[c - 32] : font5x7[0];   /* ' ' */

    for (int y = 0; y < FB_CELL_H; y++) {
        uint32_t *p = shadow_px(x0, y0 + y);
        int grow = (y - 1) >> 1;              /* 1px top pad, glyph 2x -> 14px */
        unsigned bits = (y >= 1 && grow < 7) ? glyph[grow] : 0;
        for (int x = 0; x < FB_CELL_W; x++) {
            int gcol = (x - 1) >> 1;          /* 1px left pad, glyph 2x -> 10px */
            int on = (x >= 1 && gcol < 5) && (bits & (0x10u >> gcol));
            p[x] = on ? fg : bg;
        }
    }
    if (g_batch) return;                  /* bulk repaint flushes once */
    /* Immediate: this cell's strip to VRAM (write-only, small). */
    uint64_t off = (uint64_t)y0 * g_pitch + (uint64_t)x0 * 4;
    for (int y = 0; y < FB_CELL_H; y++) {
        fb_copy8((void *)(g_vram + off), g_shadow + off, (FB_CELL_W * 4) >> 3);
        off += g_pitch;
    }
}

void fb_batch(int on) { g_batch = on; }

/* ---- gfx primitives (shadow only) ---- */

/* Clip rectangle (screen coords, [x0,x1) x [y0,y1)); every primitive
 * intersects with it. Reset = full screen. */
static int g_cx0, g_cy0, g_cx1, g_cy1;

void fb_gfx_clip(int x, int y, int w, int h) {
    g_cx0 = x < 0 ? 0 : x;
    g_cy0 = y < 0 ? 0 : y;
    g_cx1 = x + w > (int)g_w ? (int)g_w : x + w;
    g_cy1 = y + h > (int)g_h ? (int)g_h : y + h;
    if (g_cx1 < g_cx0) g_cx1 = g_cx0;
    if (g_cy1 < g_cy0) g_cy1 = g_cy0;
}

void fb_gfx_clip_reset(void) {
    g_cx0 = 0; g_cy0 = 0; g_cx1 = (int)g_w; g_cy1 = (int)g_h;
}

void fb_gfx_clear(int r, int g, int b) {
    if (!g_active) return;
    uint32_t px = fb_pack(r, g, b);
    uint64_t pat = ((uint64_t)px << 32) | px;
    fb_fill8(g_shadow, pat, ((uint64_t)g_pitch * g_h) >> 3);
}

static void blend_px(uint32_t *p, uint32_t fg, int a) {
    /* src-over on the shadow (RAM read, cheap). Per-channel via the
     * position masks; sizes are 8 everywhere we care about. */
    uint32_t dst = *p;
    uint32_t out = 0;
    int shifts[3] = { g_rpos, g_gpos, g_bpos };
    for (int i = 0; i < 3; i++) {
        int s = shifts[i];
        uint32_t fc = (fg >> s) & 0xFF, dc = (dst >> s) & 0xFF;
        out |= (((fc * (uint32_t)a) + dc * (uint32_t)(255 - a)) / 255) << s;
    }
    *p = out;
}

void fb_gfx_fill(int x, int y, int w, int h, int r, int g, int b, int a) {
    if (!g_active || a <= 0) return;
    int x1 = x + w, y1 = y + h;
    if (x < g_cx0) x = g_cx0;
    if (y < g_cy0) y = g_cy0;
    if (x1 > g_cx1) x1 = g_cx1;
    if (y1 > g_cy1) y1 = g_cy1;
    if (x >= x1 || y >= y1) return;
    uint32_t px = fb_pack(r, g, b);
    if (a >= 255) {
        /* qword fill per row when the span is even-aligned; else dword loop */
        for (int yy = y; yy < y1; yy++) {
            uint32_t *p = shadow_px(x, yy);
            int n = x1 - x;
            if (((uintptr_t)p & 7) == 0 && (n & 1) == 0) {
                fb_fill8(p, ((uint64_t)px << 32) | px, (uint64_t)n >> 1);
            } else {
                for (int i = 0; i < n; i++) p[i] = px;
            }
        }
    } else {
        for (int yy = y; yy < y1; yy++) {
            uint32_t *p = shadow_px(x, yy);
            for (int i = 0; i < x1 - x; i++) blend_px(&p[i], px, a);
        }
    }
}

void fb_gfx_point(int x, int y, int r, int g, int b) {
    if (!g_active || x < g_cx0 || y < g_cy0 || x >= g_cx1 || y >= g_cy1) return;
    *shadow_px(x, y) = fb_pack(r, g, b);
}

void fb_gfx_line(int x0, int y0, int x1, int y1, int r, int g, int b) {
    /* Bresenham through fb_gfx_point (clips per pixel). */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;  /* dy <= 0 */
    int err = dx + dy;
    for (;;) {
        fb_gfx_point(x0, y0, r, g, b);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void fb_gfx_circle(int cx, int cy, int rad, int r, int g, int b) {
    /* Filled, by horizontal spans (matches hosted gfx_circle's look). */
    if (rad < 0) return;
    for (int dy = -rad; dy <= rad; dy++) {
        int rem = rad * rad - dy * dy;
        int half = 0;
        while ((half + 1) * (half + 1) <= rem) half++;
        fb_gfx_fill(cx - half, cy + dy, 2 * half + 1, 1, r, g, b, 255);
    }
}

void fb_gfx_text(int x, int y, const char *s, int r, int g, int b, int scale) {
    if (!g_active || !s) return;
    if (scale < 1) scale = 1;
    int cx = x;
    for (; *s; s++) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126) ch = '?';
        const unsigned char *glyph = font5x7[ch - 32];
        for (int row = 0; row < 7; row++) {
            unsigned bits = glyph[row];
            for (int col = 0; col < 5; col++)
                if (bits & (0x10u >> col))
                    fb_gfx_fill(cx + col * scale, y + row * scale,
                                scale, scale, r, g, b, 255);
        }
        cx += (5 + 1) * scale;                /* hosted spacing: 5px + 1 gap */
    }
}

uint32_t fb_vram_pixel(int x, int y) {
    if (!g_active || x < 0 || y < 0 || x >= (int)g_w || y >= (int)g_h) return 0;
    return *(volatile uint32_t *)(g_vram + (uint64_t)y * g_pitch + (uint64_t)x * 4);
}
