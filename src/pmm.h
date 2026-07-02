/* Physical memory manager — 4 KiB page bitmap over the Multiboot2
 * memory map. This is the substrate of the page_alloc HAL root that
 * EigenScript's freestanding heap will stand on (M5). */
#ifndef EIGENOS_PMM_H
#define EIGENOS_PMM_H

#include <stdint.h>

/* Walk the MB2 memory map and build the allocator. Returns 0 on
 * success, negative if the mmap tag is missing/malformed. */
int pmm_init(uint32_t mb2_info);

/* Allocate/free one 4 KiB physical page. Returns the physical address
 * (page-aligned, nonzero) or 0 when out of pages. Freeing 0 or an
 * unallocated page is ignored. */
uint64_t pmm_alloc_page(void);
void     pmm_free_page(uint64_t addr);

/* Stats for display and tests. */
uint64_t pmm_total_pages(void);
uint64_t pmm_free_pages(void);
uint64_t pmm_ram_bytes(void);        /* usable RAM the manager covers */
uint64_t pmm_highest_addr(void);     /* top of the highest usable region */
int      pmm_region_count(void);     /* available regions seen in the map */

#endif /* EIGENOS_PMM_H */
