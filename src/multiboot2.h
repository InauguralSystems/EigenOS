/* Multiboot2 boot-information structures — just the tags M2 consumes.
 * Spec: GNU Multiboot2, boot information format. GRUB hands the info
 * block's physical address to kernel_main via the boot stub (ebx->edi).
 * All tags are 8-byte aligned; iterate by (size + 7) & ~7. */
#ifndef EIGENOS_MULTIBOOT2_H
#define EIGENOS_MULTIBOOT2_H

#include <stdint.h>

#define MB2_BOOT_MAGIC 0x36d76289u   /* in eax at handoff (spec 3.3) */

#define MB2_TAG_END        0
#define MB2_TAG_MMAP       6

/* Info block header: total_size covers header + all tags. */
typedef struct {
    uint32_t total_size;
    uint32_t reserved;
} mb2_info_header;

typedef struct {
    uint32_t type;
    uint32_t size;
} mb2_tag;

#define MB2_MMAP_AVAILABLE 1         /* E820 usable RAM */

typedef struct {
    uint64_t base_addr;
    uint64_t length;
    uint32_t type;
    uint32_t reserved;
} mb2_mmap_entry;

typedef struct {
    uint32_t type;                   /* == MB2_TAG_MMAP */
    uint32_t size;                   /* header + entries */
    uint32_t entry_size;
    uint32_t entry_version;
    /* mb2_mmap_entry entries[]; */
} mb2_tag_mmap;

#endif /* EIGENOS_MULTIBOOT2_H */
