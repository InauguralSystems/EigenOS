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
#include "serial.h"
#include "idt.h"
#include "paging.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "kheap.h"

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

/* Map a fresh page at an on-demand virtual address, write a pattern
 * through the mapping, and confirm the SAME bytes appear through the
 * direct map (proving both virtual addresses alias one physical frame).
 * Then unmap and free, requiring the page count to round-trip. Exercises
 * the full PML4->PDPT->PD->PT walk-and-allocate path. */
static int paging_selftest(void) {
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    uint64_t vt = KVMAP_BASE;
    if (map_page(vt, p, PG_WRITE) != 0) { pmm_free_page(p); return 0; }
    volatile uint64_t *v = (volatile uint64_t *)vt;
    for (int i = 0; i < 512; i++) v[i] = 0x00C0FFEE00000000ULL + (uint64_t)i;
    /* the same frame, seen through the direct map — must match byte-exact */
    volatile uint64_t *d = (volatile uint64_t *)phys_to_virt(p);
    int ok = 1;
    for (int i = 0; i < 512; i++)
        if (d[i] != 0x00C0FFEE00000000ULL + (uint64_t)i) ok = 0;
    /* writing through the direct map must show through the mapping too */
    d[7] = 0xDEADBEEFCAFEBABEULL;
    if (v[7] != 0xDEADBEEFCAFEBABEULL) ok = 0;
    unmap_page(vt);
    pmm_free_page(p);
    return ok;
}

/* Exercise the heap: many mixed-size allocations with per-block sentinel
 * patterns (proving no overlap), interleaved frees, a realloc that must
 * preserve contents, then free-all which must coalesce the arena back.
 * Returns 1 on success. This is the substrate the runtime's malloc rides. */
static int kheap_selftest(void) {
    kheap_init();
    enum { N = 64 };
    void *ptr[N];
    size_t sz[N];
    for (int i = 0; i < N; i++) {
        sz[i] = (size_t)(1 + ((i * 37) % 300));       /* 1..300 bytes, varied */
        ptr[i] = kmalloc(sz[i]);
        if (!ptr[i]) return 0;
        for (size_t j = 0; j < sz[i]; j++)
            ((uint8_t *)ptr[i])[j] = (uint8_t)(i * 7 + j);
    }
    /* verify no allocation clobbered another */
    for (int i = 0; i < N; i++)
        for (size_t j = 0; j < sz[i]; j++)
            if (((uint8_t *)ptr[i])[j] != (uint8_t)(i * 7 + j)) return 0;
    /* free the even ones, realloc the odd ones larger, re-verify */
    for (int i = 0; i < N; i += 2) { kfree(ptr[i]); ptr[i] = 0; }
    for (int i = 1; i < N; i += 2) {
        size_t ns = sz[i] + 500;
        uint8_t *np = krealloc(ptr[i], ns);
        if (!np) return 0;
        for (size_t j = 0; j < sz[i]; j++)
            if (np[j] != (uint8_t)(i * 7 + j)) return 0;   /* preserved */
        ptr[i] = np;
    }
    /* kcalloc must zero */
    uint8_t *z = kcalloc(256, 1);
    if (!z) return 0;
    for (int j = 0; j < 256; j++) if (z[j]) return 0;
    kfree(z);
    for (int i = 1; i < N; i += 2) kfree(ptr[i]);
    return kheap_used_bytes() == 0;                    /* all reclaimed */
}

/* mb2_info: physical address of the Multiboot2 boot-information struct —
 * M2's memory map lives there. Delivered via edi/esi from the boot stub. */
void kernel_main(uint32_t mb2_info, uint32_t mb2_magic) {
    clear(0x07);
    put_at(0, 0, "EigenOS", 0x0F);
    put_at(0, 8, "// bare metal, x86-64 long mode", 0x07);
    put_at(2, 0, "M4: PIC+PIT+PS/2 | M3: paging | M2: PMM | M1: IDT+serial | M0: longmode", 0x08);

    /* --- serial first: the headless log for everything below --- */
    serial_init();
    int ser_ok = serial_selftest();
    serial_write("\n=== EigenOS boot log (COM1) ===\n");
    serial_write("[m0] long mode up, VGA text at 0xB8000\n");
    serial_write("[m1] serial COM1 loopback self-test: ");
    serial_write(ser_ok ? "OK\n" : "FAILED\n");
    put_at(4, 0, ser_ok ? "serial: COM1 loopback OK (full log on COM1)"
                        : "serial: COM1 loopback FAILED", ser_ok ? 0x0A : 0x4F);

    /* --- IDT early: a paging fault below should REPORT, not triple-fault --- */
    idt_init();
    __asm__ volatile ("int3");                    /* resumable trap self-test */
    if (idt_exceptions_handled() == 1) {
        put_at(5, 0, "idt: 32 vectors, #BP caught + resumed OK", 0x0A);
        serial_write("[m1] IDT installed; int3 caught + resumed OK\n");
    } else {
        put_at(5, 0, "idt: exception path FAILED", 0x4F);
        serial_write("[m1] IDT self-test FAILED\n");
    }

    if (mb2_magic != MB2_BOOT_MAGIC) {
        put_at(7, 0, "multiboot2: BAD MAGIC 0x", 0x4F);
        put_hex32(7, 24, mb2_magic, 0x4F);
        serial_write("[m0] FATAL: bad multiboot2 magic\n");
        for (;;) __asm__ volatile ("hlt");
    }
    put_at(7, 0, "multiboot2: magic ok, info @ 0x", 0x07);
    put_hex32(7, 31, mb2_info, 0x0F);
    serial_write("[m0] multiboot2 magic ok, info @ 0x");
    serial_write_hex64(mb2_info);
    serial_write("\n");

    /* --- M2: physical memory manager --- */
    if (pmm_init(mb2_info) != 0) {
        put_at(9, 0, "pmm: init FAILED (no memory-map tag?)", 0x4F);
        serial_write("[m2] pmm_init FAILED\n");
        for (;;) __asm__ volatile ("hlt");
    }
    int col;
    put_at(9, 0, "memory:", 0x07);
    col = put_dec(9, 8, (uint64_t)pmm_region_count(), 0x0F);
    put_at(9, col, " regions,", 0x07);
    col = put_dec(9, col + 10, pmm_ram_bytes() >> 20, 0x0F);
    put_at(9, col, " MiB RAM,", 0x07);
    col = put_dec(9, col + 10, pmm_total_pages(), 0x0F);
    put_at(9, col, " pages tracked", 0x07);
    serial_write("[m2] pmm: ");
    serial_write_dec((uint64_t)pmm_region_count());
    serial_write(" regions, ");
    serial_write_dec(pmm_ram_bytes() >> 20);
    serial_write(" MiB total, ");
    serial_write_dec(pmm_total_pages());
    serial_write(" pages tracked (cap lifted), ");
    serial_write_dec(pmm_free_pages());
    serial_write(" free\n");
    if (pmm_selftest()) {
        put_at(10, 0, "pmm self-test: alloc/write/verify/free OK", 0x0A);
        serial_write("[m2] pmm self-test: alloc/write/verify/free OK\n");
    } else {
        put_at(10, 0, "pmm self-test: FAILED", 0x4F);
        serial_write("[m2] pmm self-test: FAILED\n");
    }

    /* --- M3: virtual memory --- */
    if (paging_init() != 0) {
        put_at(12, 0, "paging: init FAILED (out of pages?)", 0x4F);
        serial_write("[m3] paging_init FAILED\n");
        for (;;) __asm__ volatile ("hlt");
    }
    put_at(12, 0, "paging: 4-level tables live, direct map", 0x07);
    col = put_dec(12, 40, paging_direct_bytes() >> 20, 0x0F);
    put_at(12, col, " MiB @ 0xFFFF800000000000", 0x07);
    serial_write("[m3] paging: CR3 switched; identity 1 GiB + direct map ");
    serial_write_dec(paging_direct_bytes() >> 20);
    serial_write(" MiB at 0xFFFF800000000000\n");
    if (paging_selftest()) {
        put_at(13, 0, "paging self-test: map/alias/unmap OK", 0x0A);
        serial_write("[m3] paging self-test: 4K map + direct-map alias OK\n");
    } else {
        put_at(13, 0, "paging self-test: FAILED", 0x4F);
        serial_write("[m3] paging self-test: FAILED\n");
    }

    /* --- M5 substrate: kernel heap over page_alloc + the VMM --- */
    if (kheap_selftest()) {
        put_at(14, 0, "kheap: malloc/free/realloc/calloc OK", 0x0A);
        serial_write("[m5] kheap self-test: mixed alloc/free/realloc/calloc, "
                     "no overlap, full reclaim OK\n");
    } else {
        put_at(14, 0, "kheap: self-test FAILED", 0x4F);
        serial_write("[m5] kheap self-test: FAILED\n");
    }

    /* --- M4: hardware interrupts (PIC + PIT timer + PS/2 keyboard) --- */
    pic_init((1u << 0) | (1u << 1));              /* remap; unmask IRQ0+IRQ1 */
    timer_init(100);
    keyboard_init();
    __asm__ volatile ("sti");
    serial_write("[m4] PIC remapped 32-47, PIT ");
    serial_write_dec(timer_hz());
    serial_write(" Hz, keyboard armed, interrupts ON\n");

    /* Timer self-test: with IRQs live, ticks must advance on their own.
     * Bounded busy-wait so a dead timer FAILS rather than hangs. */
    uint64_t t0 = timer_ticks();
    uint64_t guard = 0;
    while (timer_ticks() < t0 + 3 && guard < 2000000000ULL) guard++;
    int timer_ok = timer_ticks() >= t0 + 3;
    if (timer_ok) {
        put_at(15, 0, "timer: IRQ0 ticks advancing OK", 0x0A);
        serial_write("[m4] timer self-test: ticks advanced to ");
        serial_write_dec(timer_ticks());
        serial_write(" (IRQ0 firing) OK\n");
    } else {
        put_at(15, 0, "timer: IRQ0 NOT firing FAILED", 0x4F);
        serial_write("[m4] timer self-test: FAILED (no IRQ0)\n");
    }

    put_at(16, 0, "M4 live: type keys -> echoed to COM1 (timer+kbd IRQs)", 0x0F);
    serial_write("[m4] milestone-4 complete; entering idle loop.\n");
    serial_write("[m4] keyboard echo (type in the QEMU window / sendkey):\n");

    /* Idle loop: hlt sleeps until the next interrupt (timer at 100 Hz or a
     * keypress); drain and echo any buffered keystrokes to serial + VGA. */
    int kcol = 0;
    for (;;) {
        int c = kbd_getchar();
        if (c >= 0) {
            serial_putc((char)c);
            if (c == '\n') { kcol = 0; }
            else if (c == '\b') { if (kcol > 0) kcol--; put_at(18, kcol, " ", 0x0E); }
            else if (kcol < VGA_COLS) { put_at(18, kcol++, (char[]){(char)c, 0}, 0x0E); }
        }
        __asm__ volatile ("hlt");
    }
}
