/* Multiboot2 boot-information structures — just the tags M2 consumes.
 * Spec: GNU Multiboot2, boot information format. GRUB hands the info
 * block's physical address to kernel_main via the boot stub (ebx->edi).
 * All tags are 8-byte aligned; iterate by (size + 7) & ~7. */
#ifndef EIGENOS_MULTIBOOT2_H
#define EIGENOS_MULTIBOOT2_H

#include <stdint.h>

#define MB2_BOOT_MAGIC 0x36d76289u   /* in eax at handoff (spec 3.3) */

#define MB2_TAG_END          0
#define MB2_TAG_MMAP         6
#define MB2_TAG_FRAMEBUFFER  8

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

/* Framebuffer info (tag 8) — GRUB's answer to the header's framebuffer
 * request (M9.5). fb_type 1 = direct RGB (the linear framebuffer path);
 * 2 = EGA text (VBE couldn't oblige — keep the 0xB8000 console). Layout
 * matches GRUB's multiboot_tag_framebuffer_common + the type-1 color
 * info that follows it. */
#define MB2_FB_TYPE_RGB  1
#define MB2_FB_TYPE_TEXT 2

typedef struct {
    uint32_t type;                   /* == MB2_TAG_FRAMEBUFFER */
    uint32_t size;
    uint64_t addr;                   /* physical LFB base */
    uint32_t pitch;                  /* bytes per scanline */
    uint32_t width;                  /* pixels */
    uint32_t height;
    uint8_t  bpp;
    uint8_t  fb_type;
    uint16_t reserved;
    /* fb_type == 1 (direct RGB) color layout: */
    uint8_t  r_pos, r_size;
    uint8_t  g_pos, g_size;
    uint8_t  b_pos, b_size;
} __attribute__((packed)) mb2_tag_framebuffer;

#endif /* EIGENOS_MULTIBOOT2_H */
