/* Boot/crash log ring in physical RAM (M7 prep).
 *
 * Every byte that goes to serial is mirrored into a fixed physical ring
 * buffer. FIELD RESULT (docs/T3200-BOOT-LOG.md, 2026-07-02): on the
 * T3200, DRAM does NOT survive a warm reset (sentinel-tested), so the
 * reboot-into-Linux readback (tools/t3200_read_crashlog.sh) is DEAD on
 * that hardware — its post-mortem channels are the VGA screen + beep
 * codes. The ring remains load-bearing on the dev side: QEMU `pmemsave`
 * + the same reader script recover the full log from a live or wedged
 * guest without any reboot, byte-faithful vs serial (validated). The buffer address is part of the
 * machine contract:
 *   - EigenOS: pmm_init reserves [CRASHLOG_PHYS, +CRASHLOG_SIZE) so the
 *     heap never hands it out; the low identity map makes it writable
 *     from first instruction to halt.
 *   - Linux (target side): boot with
 *       memmap=64K\$0x3fff0000 iomem=relaxed
 *     so Linux neither recycles the region nor blocks /dev/mem reads
 *     (Ubuntu builds with STRICT_DEVMEM).
 * The address is the LAST 64 KiB UNDER 1 GiB — every prior candidate hit
 * a real, field-tested collision (docs/T3200-BOOT-LOG.md, 2026-07-02):
 *   - NOT 16 MiB (0x1000000): x86-64 Linux's default kernel load base
 *     (CONFIG_PHYSICAL_START) — the target's Linux refused to boot.
 *   - NOT 64 MiB (0x4000000): inside the kernel DECOMPRESSOR's working
 *     zone on Ubuntu 6.8 (decompressed image + workspace span well past
 *     64 MiB from the 16 MiB base) — the target hung black mid-boot.
 *   - NOT 128 MiB (0x8000000): exactly the top of QEMU's default -m 128M.
 *   - NOT 1 GiB (0x40000000): one byte PAST the boot stub's 1 GiB
 *     identity map — crashlog_init runs before M3 paging, so the ring
 *     must be identity-mapped from the first instruction. The ring ends
 *     exactly AT 0x40000000.
 * Needs QEMU -m >= 1024 (the Makefile's run/serial/dump targets pass it)
 * and sits inside the M-7317U's usable e820 (0x100000..0xbb6b0fff), far
 * above any Linux decompressor zone. */
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdint.h>

#define CRASHLOG_PHYS  0x3FFF0000ULL   /* 1 GiB - 64 KiB — see notes above */
#define CRASHLOG_SIZE  0x10000ULL      /* 64 KiB total, header included */
#define CRASHLOG_MAGIC "EIGSLOG1"      /* 8 bytes */

typedef struct {
    char     magic[8];      /* "EIGSLOG1" */
    uint32_t cap;           /* data capacity in bytes */
    uint32_t head;          /* next write offset into data[] */
    uint32_t wrapped;       /* 1 once the ring has lapped */
    uint32_t boot_count;    /* bumped by crashlog_init each boot */
    /* data[] follows */
} crashlog_hdr;

void crashlog_init(void);        /* claim/refresh the header (call FIRST) */
void crashlog_putc(char c);      /* append one byte (no-op before init) */

#endif
