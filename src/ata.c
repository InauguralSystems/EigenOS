/* M10: ATA PIO block driver.
 *
 * The niche constraint shapes the design: this is the disk under a
 * log-structured, replayable store — correctness and bounded behavior
 * beat throughput, so it is POLLED PIO, one sector per command, no IRQ
 * path at all (the device's nIEN is held high and IRQ14/15 never get
 * unmasked). Every wait is iteration-bounded: a dead or absent drive
 * produces a FAILED verdict, never a hung boot (port reads run ~1us,
 * so the 1M-iteration bound is roughly a one-second ceiling per wait).
 *
 * Probe order: primary master/slave, then secondary master/slave; the
 * first ATA disk wins. ATAPI devices (the boot CD answers IDENTIFY
 * with the 0x14/0xEB signature) are recognized and skipped. LBA48 is
 * used when the identify data advertises it, LBA28 otherwise — the
 * T3200's 320GB spinner needs 48-bit addressing past 128GiB.
 *
 * SAFETY: this file is mechanism only. Nothing in the kernel writes
 * to the disk on its own — the boot self-test is read-only, and the
 * EigenScript-visible write builtin (hal.c) refuses until explicitly
 * unlocked, because on the T3200 the primary disk carries the Linux
 * install this machine dual-boots from. */

#include <stdint.h>
#include "io.h"
#include "ata.h"

/* channel register offsets from the I/O base */
#define R_DATA    0
#define R_ERR     1
#define R_SECCNT  2
#define R_LBA0    3
#define R_LBA1    4
#define R_LBA2    5
#define R_DRIVE   6
#define R_STATUS  7   /* read: status, write: command */

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ28     0x20
#define CMD_READ48     0x24
#define CMD_WRITE28    0x30
#define CMD_WRITE48    0x34
#define CMD_FLUSH      0xE7
#define CMD_FLUSH48    0xEA
#define CMD_IDENTIFY   0xEC

#define CTL_NIEN 0x02

#define WAIT_BOUND 1000000UL   /* ~1s of port reads; dead drive = FAIL */

static struct {
    int      present;
    uint16_t base, ctrl;
    int      slave;
    int      lba48;
    uint64_t sectors;
    char     model[41];
} g_disk;

static void io_delay400(uint16_t ctrl) {
    /* 4 alternate-status reads = the canonical 400ns settle */
    (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl); (void)inb(ctrl);
}

static int wait_clear_bsy(uint16_t base) {
    for (unsigned long i = 0; i < WAIT_BOUND; i++)
        if (!(inb(base + R_STATUS) & ST_BSY)) return 1;
    return 0;
}

/* wait for DRQ with BSY clear; ERR/DF are hard failures */
static int wait_drq(uint16_t base) {
    for (unsigned long i = 0; i < WAIT_BOUND; i++) {
        uint8_t s = inb(base + R_STATUS);
        if (s & ST_BSY) continue;
        if (s & (ST_ERR | ST_DF)) return 0;
        if (s & ST_DRQ) return 1;
    }
    return 0;
}

static void read_words(uint16_t base, uint16_t *dst, int words) {
    for (int i = 0; i < words; i++) dst[i] = inw(base + R_DATA);
}

static void write_words(uint16_t base, const uint16_t *src, int words) {
    for (int i = 0; i < words; i++) outw(base + R_DATA, src[i]);
}

/* IDENTIFY one position; fills g_disk and returns 1 on an ATA disk */
static int probe(uint16_t base, uint16_t ctrl, int slave) {
    outb(ctrl, CTL_NIEN);                       /* no drive IRQs, ever */
    outb(base + R_DRIVE, 0xA0 | (slave ? 0x10 : 0));
    io_delay400(ctrl);
    if (inb(base + R_STATUS) == 0xFF) return 0; /* floating bus: no channel */

    outb(base + R_SECCNT, 0);
    outb(base + R_LBA0, 0);
    outb(base + R_LBA1, 0);
    outb(base + R_LBA2, 0);
    outb(base + R_STATUS, CMD_IDENTIFY);
    io_delay400(ctrl);
    if (inb(base + R_STATUS) == 0) return 0;    /* no device here */
    if (!wait_clear_bsy(base)) return 0;

    /* ATAPI (the CD) aborts IDENTIFY and leaves its signature */
    uint8_t sig1 = inb(base + R_LBA1), sig2 = inb(base + R_LBA2);
    if (sig1 == 0x14 && sig2 == 0xEB) return 0; /* packet device: skip */
    if (sig1 == 0x69 && sig2 == 0x96) return 0; /* SATA packet: skip */
    if (!wait_drq(base)) return 0;

    uint16_t id[256];
    read_words(base, id, 256);

    g_disk.present = 1;
    g_disk.base = base;
    g_disk.ctrl = ctrl;
    g_disk.slave = slave;
    g_disk.lba48 = (id[83] & (1u << 10)) ? 1 : 0;
    if (g_disk.lba48)
        g_disk.sectors = (uint64_t)id[100] | ((uint64_t)id[101] << 16) |
                         ((uint64_t)id[102] << 32) | ((uint64_t)id[103] << 48);
    else
        g_disk.sectors = (uint64_t)id[60] | ((uint64_t)id[61] << 16);

    /* model: words 27..46, big-endian byte pairs, space-padded */
    for (int i = 0; i < 20; i++) {
        g_disk.model[i * 2]     = (char)(id[27 + i] >> 8);
        g_disk.model[i * 2 + 1] = (char)(id[27 + i] & 0xFF);
    }
    g_disk.model[40] = 0;
    for (int i = 39; i >= 0 && g_disk.model[i] == ' '; i--) g_disk.model[i] = 0;
    return 1;
}

void ata_init(void) {
    g_disk.present = 0;
    if (probe(0x1F0, 0x3F6, 0)) return;
    if (probe(0x1F0, 0x3F6, 1)) return;
    if (probe(0x170, 0x376, 0)) return;
    (void)probe(0x170, 0x376, 1);
}

int         ata_present(void)  { return g_disk.present; }
uint64_t    ata_sectors(void)  { return g_disk.present ? g_disk.sectors : 0; }
const char *ata_model(void)    { return g_disk.model; }

/* select the disk and program a one-sector LBA; returns the command */
static uint8_t setup_lba(uint64_t lba, uint8_t cmd28, uint8_t cmd48) {
    uint16_t base = g_disk.base;
    if (g_disk.lba48) {
        outb(base + R_DRIVE, 0x40 | (g_disk.slave ? 0x10 : 0));
        io_delay400(g_disk.ctrl);
        outb(base + R_SECCNT, 0);                       /* count high */
        outb(base + R_LBA0, (uint8_t)(lba >> 24));
        outb(base + R_LBA1, (uint8_t)(lba >> 32));
        outb(base + R_LBA2, (uint8_t)(lba >> 40));
        outb(base + R_SECCNT, 1);                       /* count low */
        outb(base + R_LBA0, (uint8_t)lba);
        outb(base + R_LBA1, (uint8_t)(lba >> 8));
        outb(base + R_LBA2, (uint8_t)(lba >> 16));
        return cmd48;
    }
    outb(base + R_DRIVE, 0xE0 | (g_disk.slave ? 0x10 : 0) |
                         (uint8_t)((lba >> 24) & 0x0F));
    io_delay400(g_disk.ctrl);
    outb(base + R_SECCNT, 1);
    outb(base + R_LBA0, (uint8_t)lba);
    outb(base + R_LBA1, (uint8_t)(lba >> 8));
    outb(base + R_LBA2, (uint8_t)(lba >> 16));
    return cmd28;
}

int ata_read(uint64_t lba, uint8_t *buf512) {
    if (!g_disk.present || lba >= g_disk.sectors) return 0;
    if (!g_disk.lba48 && lba >= (1ULL << 28)) return 0;
    uint16_t base = g_disk.base;
    if (!wait_clear_bsy(base)) return 0;
    uint8_t cmd = setup_lba(lba, CMD_READ28, CMD_READ48);
    outb(base + R_STATUS, cmd);
    if (!wait_drq(base)) return 0;
    read_words(base, (uint16_t *)buf512, 256);
    return 1;
}

int ata_write(uint64_t lba, const uint8_t *buf512) {
    if (!g_disk.present || lba >= g_disk.sectors) return 0;
    if (!g_disk.lba48 && lba >= (1ULL << 28)) return 0;
    uint16_t base = g_disk.base;
    if (!wait_clear_bsy(base)) return 0;
    uint8_t cmd = setup_lba(lba, CMD_WRITE28, CMD_WRITE48);
    outb(base + R_STATUS, cmd);
    if (!wait_drq(base)) return 0;
    write_words(base, (const uint16_t *)buf512, 256);
    if (!wait_clear_bsy(base)) return 0;
    return (inb(base + R_STATUS) & (ST_ERR | ST_DF)) ? 0 : 1;
}

int ata_flush(void) {
    if (!g_disk.present) return 0;
    uint16_t base = g_disk.base;
    if (!wait_clear_bsy(base)) return 0;
    outb(base + R_DRIVE, (g_disk.lba48 ? 0x40 : 0xE0) |
                         (g_disk.slave ? 0x10 : 0));
    io_delay400(g_disk.ctrl);
    outb(base + R_STATUS, g_disk.lba48 ? CMD_FLUSH48 : CMD_FLUSH);
    if (!wait_clear_bsy(base)) return 0;
    return (inb(base + R_STATUS) & (ST_ERR | ST_DF)) ? 0 : 1;
}
