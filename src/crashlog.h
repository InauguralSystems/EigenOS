/* Persistent-RAM boot/crash log (M7 prep).
 *
 * Every byte that goes to serial is mirrored into a fixed physical ring
 * buffer. On a machine with no serial port (the T3200), a hang is
 * debugged by warm-rebooting into the resident Linux and reading the
 * buffer back via /dev/mem (tools/t3200_read_crashlog.sh) — DRAM
 * contents survive a warm reset. The buffer address is part of the
 * machine contract:
 *   - EigenOS: pmm_init reserves [CRASHLOG_PHYS, +CRASHLOG_SIZE) so the
 *     heap never hands it out; the low identity map makes it writable
 *     from first instruction to halt.
 *   - Linux (target side): boot with
 *       memmap=64K\$0x4000000 iomem=relaxed
 *     so Linux neither recycles the region nor blocks /dev/mem reads
 *     (Ubuntu builds with STRICT_DEVMEM).
 * The address is 64 MiB — chosen to dodge two real collisions:
 *   - NOT 16 MiB (0x1000000): that is x86-64 Linux's default kernel load
 *     base (CONFIG_PHYSICAL_START); reserving it made the T3200's Linux
 *     fail to boot ("Physical KASLR disabled: no suitable memory
 *     region!") — see docs/T3200-BOOT-LOG.md 2026-07-02.
 *   - NOT 128 MiB (0x8000000): that is exactly the top of QEMU's default
 *     -m 128M, so the ring would sit outside RAM in every default-memory
 *     QEMU gate run.
 * 64 MiB is inside usable RAM on QEMU (any -m >= 128M) and on the
 * M-7317U e820 (usable 0x100000..0xbb6b0fff), and clear of the kernel
 * image + .bss (~5 MiB end). */
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdint.h>

#define CRASHLOG_PHYS  0x04000000ULL   /* 64 MiB — see collision notes above */
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
