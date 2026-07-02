/* Physical memory manager (milestone-2).
 *
 * A fixed 32 KiB bitmap tracks 4 KiB pages up to 1 GiB — exactly the
 * range the boot stub identity-maps, so every page the allocator hands
 * out is already addressable. RAM beyond 1 GiB (the T3200 has 3.8 GiB)
 * stays untracked until M3 brings real virtual memory; the cap is a
 * deliberate M2 simplification, surfaced in the stats so it can't be
 * mistaken for the machine's true size.
 *
 * Reserved and never handed out: everything below 1 MiB (BIOS/VGA/low
 * memory), the kernel image ([_kernel_start, _kernel_end), symbols from
 * linker.ld — page tables and the boot stack live in .bss so they're
 * covered), and the Multiboot2 info block GRUB parked in memory.
 */
#include "pmm.h"
#include "multiboot2.h"

#define PAGE_SIZE   4096ULL
#define MAX_BYTES   (1ULL << 30)                 /* 1 GiB identity-map cap */
#define MAX_PAGES   (MAX_BYTES / PAGE_SIZE)      /* 262144 */
#define BITMAP_LEN  (MAX_PAGES / 64)             /* 4096 u64s = 32 KiB */

/* Bit set = page FREE. Starts all-zero (.bss): everything reserved
 * until the memory map proves a page is usable. */
static uint64_t g_bitmap[BITMAP_LEN];
static uint64_t g_total;                          /* pages ever marked free */
static uint64_t g_free;
static uint64_t g_ram_bytes;
static int      g_regions;

extern char _kernel_start[], _kernel_end[];       /* linker.ld */

static void mark_free(uint64_t page) {
    uint64_t w = page / 64, b = page % 64;
    if (!(g_bitmap[w] & (1ULL << b))) {
        g_bitmap[w] |= 1ULL << b;
        g_total++;
        g_free++;
    }
}

static void mark_used_range(uint64_t start, uint64_t end) {  /* [start, end) bytes */
    uint64_t p0 = start / PAGE_SIZE;
    uint64_t p1 = (end + PAGE_SIZE - 1) / PAGE_SIZE;
    if (p1 > MAX_PAGES) p1 = MAX_PAGES;
    for (uint64_t p = p0; p < p1; p++) {
        uint64_t w = p / 64, b = p % 64;
        if (g_bitmap[w] & (1ULL << b)) {
            g_bitmap[w] &= ~(1ULL << b);
            g_total--;
            g_free--;
        }
    }
}

int pmm_init(uint32_t mb2_info) {
    const mb2_info_header *info = (const mb2_info_header *)(uint64_t)mb2_info;
    if (!mb2_info || info->total_size < sizeof(*info)) return -1;

    const uint8_t *p   = (const uint8_t *)info + sizeof(*info);
    const uint8_t *end = (const uint8_t *)info + info->total_size;
    const mb2_tag_mmap *mmap = 0;

    while (p + sizeof(mb2_tag) <= end) {
        const mb2_tag *tag = (const mb2_tag *)p;
        if (tag->type == MB2_TAG_END) break;
        if (tag->type == MB2_TAG_MMAP) mmap = (const mb2_tag_mmap *)tag;
        if (tag->size < sizeof(mb2_tag)) return -2;          /* malformed */
        p += (tag->size + 7) & ~7u;
    }
    if (!mmap || mmap->entry_size < sizeof(mb2_mmap_entry)) return -3;

    const uint8_t *e   = (const uint8_t *)mmap + sizeof(*mmap);
    const uint8_t *eend = (const uint8_t *)mmap + mmap->size;
    for (; e + mmap->entry_size <= eend; e += mmap->entry_size) {
        const mb2_mmap_entry *m = (const mb2_mmap_entry *)e;
        if (m->type != MB2_MMAP_AVAILABLE) continue;
        g_regions++;
        g_ram_bytes += m->length;
        /* free whole pages inside [base, base+length), >= 1 MiB, < cap */
        uint64_t lo = m->base_addr;
        uint64_t hi = m->base_addr + m->length;
        if (lo < 0x100000) lo = 0x100000;
        if (hi > MAX_BYTES) hi = MAX_BYTES;
        lo = (lo + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);        /* round in */
        hi = hi & ~(PAGE_SIZE - 1);
        for (uint64_t a = lo; a < hi; a += PAGE_SIZE)
            mark_free(a / PAGE_SIZE);
    }

    /* Carve out what the map calls "available" but we occupy. */
    mark_used_range((uint64_t)_kernel_start, (uint64_t)_kernel_end);
    mark_used_range((uint64_t)mb2_info, (uint64_t)mb2_info + info->total_size);
    return 0;
}

uint64_t pmm_alloc_page(void) {
    for (uint64_t w = 0; w < BITMAP_LEN; w++) {
        if (!g_bitmap[w]) continue;
        uint64_t b = (uint64_t)__builtin_ctzll(g_bitmap[w]);
        g_bitmap[w] &= ~(1ULL << b);
        g_free--;
        return (w * 64 + b) * PAGE_SIZE;
    }
    return 0;
}

void pmm_free_page(uint64_t addr) {
    if (!addr || addr % PAGE_SIZE || addr >= MAX_BYTES) return;
    uint64_t page = addr / PAGE_SIZE;
    uint64_t w = page / 64, b = page % 64;
    if (g_bitmap[w] & (1ULL << b)) return;        /* double free: ignore */
    g_bitmap[w] |= 1ULL << b;
    g_free++;
}

uint64_t pmm_total_pages(void) { return g_total; }
uint64_t pmm_free_pages(void)  { return g_free; }
uint64_t pmm_ram_bytes(void)   { return g_ram_bytes; }
int      pmm_region_count(void){ return g_regions; }
