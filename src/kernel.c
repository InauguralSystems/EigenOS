/* EigenOS kernel entry (64-bit long mode).
 *
 * Milestone-0 proved the build + boot + long-mode pipeline (VGA text at
 * 0xB8000, identity-mapped by the boot stub). Milestone-2 adds the
 * physical memory manager: parse the Multiboot2 memory map, run a page
 * allocator over it, and prove it with an on-screen self-test. No libc,
 * no runtime — toward running the EigenScript runtime freestanding. */
#include <stdint.h>
#include "pmm.h"
#include "multiboot2.h"

#define VGA       ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

/* attr<<8 | char : light-grey-on-black (0x07), white-on-black (0x0F) */
static inline uint16_t cell(char c, uint8_t attr) {
    return (uint16_t)c | ((uint16_t)attr << 8);
}

static void clear(uint8_t attr) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = cell(' ', attr);
}

static void put_at(int row, int col, const char *s, uint8_t attr) {
    int i = row * VGA_COLS + col;
    for (; *s; s++, i++)
        VGA[i] = cell(*s, attr);
}

static void put_hex32(int row, int col, uint32_t v, uint8_t attr) {
    static const char digits[] = "0123456789abcdef";
    int i = row * VGA_COLS + col;
    for (int shift = 28; shift >= 0; shift -= 4, i++)
        VGA[i] = cell(digits[(v >> shift) & 0xF], attr);
}

/* Decimal u64; returns the column just past the last digit. */
static int put_dec(int row, int col, uint64_t v, uint8_t attr) {
    char tmp[21];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v) { tmp[n++] = (char)('0' + v % 10); v /= 10; }
    int i = row * VGA_COLS + col;
    for (int k = n - 1; k >= 0; k--, i++)
        VGA[i] = cell(tmp[k], attr);
    return col + n;
}

/* Allocate, pattern-fill, verify, free, re-allocate — and require the
 * free count to come back to its starting value. The pages are written
 * through the identity map, so a bad allocator (overlap, misalignment,
 * handing out reserved frames) shows up as a FAIL on screen. */
static int pmm_selftest(void) {
    uint64_t before = pmm_free_pages();
    uint64_t a = pmm_alloc_page(), b = pmm_alloc_page(), c = pmm_alloc_page();
    if (!a || !b || !c) return 0;
    if (a == b || b == c || a == c) return 0;
    if ((a | b | c) & 0xFFF) return 0;
    volatile uint64_t *pa = (volatile uint64_t *)a;
    volatile uint64_t *pb = (volatile uint64_t *)b;
    for (int i = 0; i < 512; i++) {
        pa[i] = 0xA5A5A5A5A5A5A5A5ULL ^ (uint64_t)i;
        pb[i] = 0x5A5A5A5A5A5A5A5AULL ^ (uint64_t)i;
    }
    for (int i = 0; i < 512; i++) {
        if (pa[i] != (0xA5A5A5A5A5A5A5A5ULL ^ (uint64_t)i)) return 0;
        if (pb[i] != (0x5A5A5A5A5A5A5A5AULL ^ (uint64_t)i)) return 0;
    }
    pmm_free_page(b);
    uint64_t d = pmm_alloc_page();
    if (!d || (d & 0xFFF)) return 0;
    pmm_free_page(a);
    pmm_free_page(c);
    pmm_free_page(d);
    return pmm_free_pages() == before;
}

/* mb2_info: physical address of the Multiboot2 boot-information struct —
 * M2's memory map lives there. Delivered via edi/esi from the boot stub. */
void kernel_main(uint32_t mb2_info, uint32_t mb2_magic) {
    clear(0x07);
    put_at(0, 0, "EigenOS", 0x0F);
    put_at(0, 8, "// bare metal, x86-64 long mode", 0x07);
    put_at(2, 0, "milestone-2: physical memory manager over the multiboot2 map.", 0x08);
    if (mb2_magic == MB2_BOOT_MAGIC) {
        put_at(4, 0, "multiboot2: magic ok, info @ 0x", 0x07);
        put_hex32(4, 31, mb2_info, 0x0F);
    } else {
        put_at(4, 0, "multiboot2: BAD MAGIC 0x", 0x4F);
        put_hex32(4, 24, mb2_magic, 0x4F);
        for (;;) __asm__ volatile ("hlt");
    }

    int rc = pmm_init(mb2_info);
    if (rc != 0) {
        put_at(6, 0, "pmm: init FAILED (no memory-map tag?)", 0x4F);
    } else {
        int col;
        put_at(6, 0, "memory:", 0x07);
        col = put_dec(6, 8, (uint64_t)pmm_region_count(), 0x0F);
        put_at(6, col, " usable regions,", 0x07);
        col = put_dec(6, col + 17, pmm_ram_bytes() >> 20, 0x0F);
        put_at(6, col, " MiB (tracking first 1 GiB)", 0x07);

        put_at(7, 0, "pmm:", 0x07);
        col = put_dec(7, 5, pmm_total_pages(), 0x0F);
        put_at(7, col, " pages,", 0x07);
        col = put_dec(7, col + 8, pmm_free_pages(), 0x0F);
        put_at(7, col, " free", 0x07);

        if (pmm_selftest())
            put_at(9, 0, "pmm self-test: alloc/write/verify/free OK", 0x0A);
        else
            put_at(9, 0, "pmm self-test: FAILED", 0x4F);
    }
    for (;;)
        __asm__ volatile ("hlt");
}
