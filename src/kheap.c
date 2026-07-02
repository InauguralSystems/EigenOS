/* Kernel heap (milestone-5).
 *
 * One contiguous virtual arena at HEAP_BASE, grown a page at a time by
 * mapping fresh PMM frames through the M3 VMM (the frames need not be
 * physically contiguous — only the virtual arena is). Blocks are an
 * implicit list of [header | payload]; first-fit allocation splits, free
 * does a full forward coalesce pass. O(n) walks — fine for a kernel heap
 * and the runtime's allocation pattern; correctness over cleverness. */
#include "kheap.h"
#include "pmm.h"
#include "paging.h"
#include <stdint.h>

/* LOW-canonical (bit 47 = 0, < 2^48) on purpose: the EigenScript runtime
 * NaN-boxes heap pointers by keeping only their low 48 bits, so malloc must
 * return addresses that survive that truncation. A higher-half heap would
 * come back non-canonical and #GP on the first deref. 16 TiB — clear of the
 * identity map, the kernel image, and the direct/KVMAP higher-half regions. */
#define HEAP_BASE  0x0000100000000000ULL
#define PAGE_SIZE  4096ULL
#define ALIGN      16ULL
#define HDR        (sizeof(block))

typedef struct block {
    size_t size;                              /* payload bytes */
    size_t free;                              /* 1 = free, 0 = in use */
} block;

static uint64_t g_top;                        /* virtual end of mapped arena */
static size_t   g_used;                       /* live payload bytes */
static int      g_ready;

static void *kmemcpy(void *d, const void *s, size_t n) {
    uint8_t *dd = d; const uint8_t *ss = s;
    for (size_t i = 0; i < n; i++) dd[i] = ss[i];
    return d;
}
static void *kmemset(void *d, int c, size_t n) {
    uint8_t *dd = d;
    for (size_t i = 0; i < n; i++) dd[i] = (uint8_t)c;
    return d;
}

/* Extend the arena by at least `need` bytes, laying a free block over the
 * newly mapped region so the implicit-list walk stays well-formed. The
 * caller coalesces it with a preceding free block if adjacent. 0 = ok. */
static int grow(size_t need) {
    uint64_t old_top = g_top;
    size_t bytes = (need + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (size_t off = 0; off < bytes; off += PAGE_SIZE) {
        uint64_t phys = pmm_alloc_page();
        if (!phys) return -1;
        if (map_page(g_top, phys, PG_WRITE) != 0) { pmm_free_page(phys); return -1; }
        g_top += PAGE_SIZE;
    }
    block *nb = (block *)old_top;
    nb->size = (size_t)(g_top - old_top) - HDR;
    nb->free = 1;
    return 0;
}

void kheap_init(void) {
    if (g_ready) return;
    g_top = HEAP_BASE;
    g_used = 0;
    if (grow(PAGE_SIZE) != 0) return;
    g_ready = 1;
}

static block *next_block(block *b) {
    uint64_t n = (uint64_t)b + HDR + b->size;
    return (n < g_top) ? (block *)n : 0;
}

static void coalesce(void) {
    block *b = (block *)HEAP_BASE;
    while (b) {
        block *n = next_block(b);
        if (b->free && n && n->free) {
            b->size += HDR + n->size;         /* absorb the next block */
            continue;                         /* retry: maybe absorb again */
        }
        b = n;
    }
}

void *kmalloc(size_t n) {
    if (!g_ready) kheap_init();
    if (n == 0) return 0;
    n = (n + ALIGN - 1) & ~(ALIGN - 1);

    for (int attempt = 0; attempt < 2; attempt++) {
        for (block *b = (block *)HEAP_BASE; b; b = next_block(b)) {
            if (!b->free || b->size < n) continue;
            if (b->size >= n + HDR + ALIGN) {           /* split off remainder */
                block *rem = (block *)((uint64_t)b + HDR + n);
                rem->size = b->size - n - HDR;
                rem->free = 1;
                b->size = n;
            }
            b->free = 0;
            g_used += b->size;
            void *payload = (void *)((uint64_t)b + HDR);
            /* Zero fresh allocations. Hosted malloc hands back fresh mmap
             * pages that are already zero, so the runtime can rely on
             * newly-allocated memory reading as 0; a reused dirty block here
             * would feed it garbage (a stale pointer -> a wild VM dispatch).
             * Matching that behavior keeps bare metal == hosted. */
            kmemset(payload, 0, n);
            return payload;
        }
        if (grow(n + HDR) != 0) return 0;               /* map more, retry once */
        coalesce();
    }
    return 0;
}

void kfree(void *p) {
    if (!p) return;
    block *b = (block *)((uint64_t)p - HDR);
    if (b->free) return;                                /* double free: ignore */
    b->free = 1;
    g_used -= b->size;
    coalesce();
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    if (size && total / size != nmemb) return 0;        /* overflow */
    void *p = kmalloc(total);
    if (p) kmemset(p, 0, total);
    return p;
}

void *krealloc(void *p, size_t n) {
    if (!p) return kmalloc(n);
    if (n == 0) { kfree(p); return 0; }
    block *b = (block *)((uint64_t)p - HDR);
    if (b->size >= n) return p;                         /* shrink-in-place */
    void *np = kmalloc(n);
    if (!np) return 0;
    kmemcpy(np, p, b->size);
    kfree(p);
    return np;
}

size_t kheap_used_bytes(void)   { return g_used; }
size_t kheap_mapped_bytes(void) { return (size_t)(g_top - HEAP_BASE); }
