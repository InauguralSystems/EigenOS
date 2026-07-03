/* M10: ATA PIO block driver — the substrate tidelog-as-filesystem
 * stands on. Polled PIO only (nIEN held; IRQ14/15 stay masked), one
 * sector per command, every wait iteration-bounded so a dead drive
 * FAILS instead of hanging the boot. */
#ifndef EIGENOS_ATA_H
#define EIGENOS_ATA_H

#include <stdint.h>

void        ata_init(void);      /* probe both channels, keep first ATA disk */
int         ata_present(void);
uint64_t    ata_sectors(void);   /* total addressable LBAs (0 if absent) */
const char *ata_model(void);     /* identify model string, trimmed */
int         ata_read(uint64_t lba, uint8_t *buf512);        /* 1 ok, 0 fail */
int         ata_write(uint64_t lba, const uint8_t *buf512); /* 1 ok, 0 fail */
int         ata_flush(void);                                /* 1 ok, 0 fail */

#endif /* EIGENOS_ATA_H */
