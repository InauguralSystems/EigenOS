/* Virtual memory (milestone-3): kernel-built 4-level page tables, a
 * higher-half direct map of all physical RAM, and 4 KiB map/unmap.
 *
 * Address-space layout after paging_init:
 *   0x0000000000000000 .. 1 GiB   identity map (kernel code/stack/VGA
 *                                  keep working across the CR3 switch)
 *   0xFFFF800000000000 + phys     direct map of all physical RAM — how
 *                                  the kernel reaches any page the PMM
 *                                  hands out (phys_to_virt below)
 *   0xFFFFC00000000000 +          on-demand kernel mappings (map_page)
 *
 * Relocating kernel *execution* into the higher half (to free the low
 * canonical half for ring-3 userland) is deferred until a userland
 * milestone actually needs it; M3 establishes the mechanism and the
 * direct map. */
#ifndef EIGENOS_PAGING_H
#define EIGENOS_PAGING_H

#include <stdint.h>

#define PHYS_MAP_BASE 0xFFFF800000000000ULL
#define KVMAP_BASE    0xFFFFC00000000000ULL   /* on-demand mapping window */

#define PG_PRESENT 0x1ULL
#define PG_WRITE   0x2ULL
#define PG_PWT     0x8ULL     /* write-through  (device MMIO)             */
#define PG_PCD     0x10ULL    /* cache-disable  (device MMIO — M13 NIC)   */
#define PG_HUGE    0x80ULL

/* Build the tables (identity low 1 GiB + direct-map all RAM) and load
 * CR3. Returns 0 on success, negative if a table allocation fails. */
int  paging_init(void);

/* Map/unmap one 4 KiB page. map_page allocates intermediate tables from
 * the PMM as needed; flags are OR'd with PG_PRESENT. Returns 0 / -1. */
int  map_page(uint64_t virt, uint64_t phys, uint64_t flags);
void unmap_page(uint64_t virt);

/* Access any physical address through the direct map. Valid only after
 * paging_init. */
static inline void *phys_to_virt(uint64_t phys) {
    return (void *)(PHYS_MAP_BASE + phys);
}

uint64_t paging_direct_bytes(void);    /* how much phys the direct map covers */

#endif /* EIGENOS_PAGING_H */
