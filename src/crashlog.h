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
 *       memmap=64K\$0x1000000 iomem=relaxed
 *     so Linux neither recycles the region nor blocks /dev/mem reads
 *     (Ubuntu builds with STRICT_DEVMEM).
 * 16 MiB is inside usable RAM on QEMU (any -m >= 32M) and on the
 * M-7317U e820 (usable 0x100000..0xbb6b0fff), and clear of the kernel
 * image + .bss (~5 MiB end). */
#ifndef CRASHLOG_H
#define CRASHLOG_H

#include <stdint.h>

#define CRASHLOG_PHYS  0x01000000ULL   /* 16 MiB */
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
