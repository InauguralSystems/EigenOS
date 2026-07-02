/* Virtual memory manager (milestone-3). */
#include "paging.h"
#include "pmm.h"

#define PAGE_SIZE 4096ULL
#define ADDR_MASK 0x000FFFFFFFFFF000ULL          /* 52-bit phys, page-aligned */

#define PML4_IDX(v) (((v) >> 39) & 0x1FF)
#define PDPT_IDX(v) (((v) >> 30) & 0x1FF)
#define PD_IDX(v)   (((v) >> 21) & 0x1FF)
#define PT_IDX(v)   (((v) >> 12) & 0x1FF)

static uint64_t g_pml4_phys;
static uint64_t g_direct_bytes;
static int      g_active;                        /* CR3 loaded? */

/* Table access. During construction (before the CR3 switch) the boot
 * stub's identity map is live, so a physical address IS a valid pointer.
 * Afterward, tables are reached through the direct map — which also
 * covers any table the PMM later hands out above 1 GiB. */
static uint64_t *tbl(uint64_t phys) {
    return g_active ? (uint64_t *)(PHYS_MAP_BASE + phys) : (uint64_t *)phys;
}

static uint64_t alloc_table(void) {
    uint64_t p = pmm_alloc_page();
    if (!p) return 0;
    uint64_t *t = tbl(p);
    for (int i = 0; i < 512; i++) t[i] = 0;
    return p;
}

int paging_init(void) {
    uint64_t ramtop = pmm_highest_addr();
    ramtop = (ramtop + 0x1FFFFFULL) & ~0x1FFFFFULL;       /* round to 2 MiB */
    if (ramtop < (1ULL << 30)) ramtop = 1ULL << 30;       /* map >= 1 GiB */

    g_pml4_phys = alloc_table();
    if (!g_pml4_phys) return -1;
    uint64_t *pml4 = tbl(g_pml4_phys);

    /* Identity-map [0, 1 GiB) with 2 MiB pages so the currently-running
     * kernel (code, stack, VGA — all < 1 GiB) survives the CR3 load. */
    uint64_t id_pdpt = alloc_table();
    uint64_t id_pd   = alloc_table();
    if (!id_pdpt || !id_pd) return -1;
    pml4[0] = id_pdpt | PG_PRESENT | PG_WRITE;      /* identity base: all idx 0 */
    tbl(id_pdpt)[0] = id_pd | PG_PRESENT | PG_WRITE;
    for (uint64_t i = 0; i < 512; i++)
        tbl(id_pd)[i] = (i * 0x200000ULL) | PG_PRESENT | PG_WRITE | PG_HUGE;

    /* Direct-map [0, ramtop) at PHYS_MAP_BASE with 2 MiB pages. */
    uint64_t dm_pdpt = alloc_table();
    if (!dm_pdpt) return -1;
    pml4[PML4_IDX(PHYS_MAP_BASE)] = dm_pdpt | PG_PRESENT | PG_WRITE;
    uint64_t gigs = (ramtop + (1ULL << 30) - 1) >> 30;
    for (uint64_t g = 0; g < gigs; g++) {
        uint64_t pd = alloc_table();
        if (!pd) return -1;
        tbl(dm_pdpt)[g] = pd | PG_PRESENT | PG_WRITE;
        for (uint64_t i = 0; i < 512; i++) {
            uint64_t phys = g * (1ULL << 30) + i * 0x200000ULL;
            tbl(pd)[i] = phys | PG_PRESENT | PG_WRITE | PG_HUGE;
        }
    }
    g_direct_bytes = gigs << 30;

    __asm__ volatile ("mov %0, %%cr3" :: "r"(g_pml4_phys) : "memory");
    g_active = 1;
    return 0;
}

/* Walk to the PT entry for `virt`, allocating PDPT/PD/PT as needed. */
static uint64_t *pt_entry(uint64_t virt, int create) {
    uint64_t *pml4 = tbl(g_pml4_phys);
    uint64_t *e = &pml4[PML4_IDX(virt)];
    if (!(*e & PG_PRESENT)) {
        if (!create) return 0;
        uint64_t np = alloc_table();
        if (!np) return 0;
        *e = np | PG_PRESENT | PG_WRITE;
    }
    uint64_t *pdpt = tbl(*e & ADDR_MASK);
    e = &pdpt[PDPT_IDX(virt)];
    if (!(*e & PG_PRESENT)) {
        if (!create) return 0;
        uint64_t np = alloc_table();
        if (!np) return 0;
        *e = np | PG_PRESENT | PG_WRITE;
    }
    uint64_t *pd = tbl(*e & ADDR_MASK);
    e = &pd[PD_IDX(virt)];
    if (*e & PG_HUGE) return 0;                  /* don't split a 2 MiB page */
    if (!(*e & PG_PRESENT)) {
        if (!create) return 0;
        uint64_t np = alloc_table();
        if (!np) return 0;
        *e = np | PG_PRESENT | PG_WRITE;
    }
    uint64_t *ptbl = tbl(*e & ADDR_MASK);
    return &ptbl[PT_IDX(virt)];
}

int map_page(uint64_t virt, uint64_t phys, uint64_t flags) {
    uint64_t *pte = pt_entry(virt, 1);
    if (!pte) return -1;
    *pte = (phys & ADDR_MASK) | flags | PG_PRESENT;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
    return 0;
}

void unmap_page(uint64_t virt) {
    uint64_t *pte = pt_entry(virt, 0);
    if (!pte || !(*pte & PG_PRESENT)) return;
    *pte = 0;
    __asm__ volatile ("invlpg (%0)" :: "r"(virt) : "memory");
}

uint64_t paging_direct_bytes(void) { return g_direct_bytes; }
