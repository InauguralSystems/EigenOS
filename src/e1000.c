/* M13: Intel 8254x (e1000) driver. See e1000.h for the model.
 *
 * The first real MMIO device (the deliberate M12 deferral): BAR0 maps
 * into the on-demand window uncached (PG_PCD|PG_PWT — QEMU is
 * indifferent, real silicon is not), descriptor rings and frame
 * buffers live in PMM pages reached through the direct map, so their
 * physical addresses are known by construction (the kheap's virtual
 * arena is deliberately NOT physically contiguous — never DMA from
 * kmalloc memory).
 *
 * Interrupts are masked (IMC = all ones) and IRQ11 stays masked at the
 * PIC: RX is a cooperative drain, exactly like the M12 claim rings —
 * the script's "interrupt callback" is a poll between evals, and IRQ
 * context never touches the runtime. */

#include <stdint.h>
#include "io.h"
#include "paging.h"
#include "pmm.h"
#include "timer.h"
#include "e1000.h"

/* ---- registers (8254x software developer's manual, table 13-2) ---- */
#define R_CTRL    0x0000
#define R_STATUS  0x0008
#define R_EERD    0x0014
#define R_MDIC    0x0020
#define R_ICR     0x00C0
#define R_IMC     0x00D8
#define R_RCTL    0x0100
#define R_TCTL    0x0400
#define R_TIPG    0x0410
#define R_RDBAL   0x2800
#define R_RDBAH   0x2804
#define R_RDLEN   0x2808
#define R_RDH     0x2810
#define R_RDT     0x2818
#define R_TDBAL   0x3800
#define R_TDBAH   0x3804
#define R_TDLEN   0x3808
#define R_TDH     0x3810
#define R_TDT     0x3818
#define R_MTA     0x5200        /* 128 dwords */
#define R_RAL0    0x5400
#define R_RAH0    0x5404

#define CTRL_ASDE   (1u << 5)
#define CTRL_SLU    (1u << 6)
#define CTRL_RST    (1u << 26)

#define RAH_AV      (1u << 31)

#define EERD_START  (1u << 0)
#define EERD_DONE   (1u << 4)   /* 82540EM microwire timing */

#define MDIC_OP_WRITE (1u << 26)
#define MDIC_OP_READ  (2u << 26)
#define MDIC_READY    (1u << 28)
#define MDIC_ERROR    (1u << 30)
#define MDIC_PHYADD   (1u << 21)          /* internal PHY = address 1 */
#define MII_BMCR      0                   /* PHY reg 0: basic control */
#define BMCR_LOOPBACK 0x4000

#define RCTL_EN     (1u << 1)
#define RCTL_BAM    (1u << 15)
#define RCTL_SECRC  (1u << 26)  /* strip CRC: RX length == TX'd frame */

#define TCTL_EN     (1u << 1)
#define TCTL_PSP    (1u << 3)
#define TCTL_CT     (0x10u << 4)
#define TCTL_COLD   (0x40u << 12)
#define TIPG_DEF    0x0060200Au /* IPGT 10, IPGR1 8, IPGR2 6 (copper) */

/* legacy descriptors */
typedef struct {
    uint64_t addr;
    uint16_t length;
    uint16_t csum;
    uint8_t  status;
    uint8_t  errors;
    uint16_t special;
} rx_desc;                       /* 16 bytes */

typedef struct {
    uint64_t addr;
    uint16_t length;
    uint8_t  cso;
    uint8_t  cmd;
    uint8_t  status;
    uint8_t  css;
    uint16_t special;
} tx_desc;                       /* 16 bytes */

#define RXD_DD   0x01
#define RXD_EOP  0x02
#define TXD_EOP  0x01
#define TXD_IFCS 0x02
#define TXD_RS   0x08
#define TXD_DD   0x01           /* status */

#define NRX 32                  /* ring lengths: multiples of 8 */
#define NTX 8
#define BUFSZ 2048              /* RCTL BSIZE 00 (BSEX 0) */

/* BAR0 maps here: the on-demand window, clear of the LFB at
 * 0xFFFFC00080000000 (a few MiB) and the paging self-test page. */
#define E1000_VIRT 0xFFFFC00100000000ULL
#define BAR_PAGES  32           /* 8254x BAR0 = 128 KiB */

#define WAIT_BOUND 1000000UL    /* ~1s of MMIO polls; dead NIC = FAIL */

static struct {
    int      up;
    uint8_t  mac[6];
    volatile rx_desc *rxr;      /* rings, via the direct map */
    volatile tx_desc *txr;
    uint64_t rxr_phys, txr_phys;
    uint8_t *rxbuf[NRX];        /* frame buffers, via the direct map */
    uint8_t *txbuf[NTX];
    uint64_t rxbuf_phys[NRX], txbuf_phys[NTX];
    unsigned rx_next;           /* next descriptor to examine */
    unsigned tx_next;           /* next descriptor to fill (== TDT) */
} g;

/* rep-string copy: immune to -O2 loop-idiom recognition emitting a
 * call into the SSE mini-libc (fb.c precedent; keeps the per-object
 * xmm/memcpy audit at zero even though this driver is never on an
 * IRQ path). */
static void ncopy(void *dst, const void *src, uint64_t n) {
    __asm__ volatile ("cld; rep movsb"
                      : "+D"(dst), "+S"(src), "+c"(n) :: "memory");
}

static inline uint32_t rr(uint32_t off) {
    return *(volatile uint32_t *)(E1000_VIRT + off);
}
static inline void wr(uint32_t off, uint32_t v) {
    *(volatile uint32_t *)(E1000_VIRT + off) = v;
}

/* ---- PCI config mechanism #1 (the C twin of demos/pci.eigs) ---- */
static uint32_t pci_cfg_read(int dev, int fn, int reg) {
    outl(0xCF8, 0x80000000u | ((uint32_t)dev << 11) |
                ((uint32_t)fn << 8) | (uint32_t)(reg & 0xFC));
    return inl(0xCFC);
}
static void pci_cfg_write(int dev, int fn, int reg, uint32_t v) {
    outl(0xCF8, 0x80000000u | ((uint32_t)dev << 11) |
                ((uint32_t)fn << 8) | (uint32_t)(reg & 0xFC));
    outl(0xCFC, v);
}

/* QEMU's 8254x models: 82540EM (the default "e1000"), 82545EM,
 * 82544GC, 82543GC. All share the legacy ring programming below. */
static int devid_supported(uint32_t id) {
    return id == 0x100E || id == 0x100F || id == 0x1008 || id == 0x1004;
}

/* ---- EEPROM (fallback MAC source when RAL/RAH is not preloaded) ---- */
static int eeprom_word(int addr, uint16_t *out) {
    wr(R_EERD, ((uint32_t)addr << 8) | EERD_START);
    for (unsigned long i = 0; i < WAIT_BOUND; i++) {
        uint32_t v = rr(R_EERD);
        if (v & EERD_DONE) { *out = (uint16_t)(v >> 16); return 1; }
    }
    return 0;
}

/* ---- PHY access over MDIC ---- */
static int phy_read(int reg, uint16_t *out) {
    wr(R_MDIC, MDIC_OP_READ | MDIC_PHYADD | ((uint32_t)reg << 16));
    for (unsigned long i = 0; i < WAIT_BOUND; i++) {
        uint32_t v = rr(R_MDIC);
        if (v & MDIC_READY) {
            if (v & MDIC_ERROR) return 0;
            *out = (uint16_t)v;
            return 1;
        }
    }
    return 0;
}
static int phy_write(int reg, uint16_t val) {
    wr(R_MDIC, MDIC_OP_WRITE | MDIC_PHYADD | ((uint32_t)reg << 16) | val);
    for (unsigned long i = 0; i < WAIT_BOUND; i++)
        if (rr(R_MDIC) & MDIC_READY) return 1;
    return 0;
}

int e1000_present(void)       { return g.up; }
const uint8_t *e1000_mac(void) { return g.mac; }

int e1000_init(void) {
    /* -- find it: bus 0, fn 0 (QEMU's default e1000 placement) -- */
    int dev = -1;
    for (int d = 0; d < 32; d++) {
        uint32_t id = pci_cfg_read(d, 0, 0);
        if ((id & 0xFFFF) != 0x8086) continue;
        uint32_t cls = pci_cfg_read(d, 0, 0x08) >> 16;    /* class:sub */
        if (cls != 0x0200) continue;                      /* ethernet  */
        if (!devid_supported(id >> 16)) continue;
        dev = d;
        break;
    }
    if (dev < 0) return 0;

    /* -- enable memory decode + bus mastering, map BAR0 uncached -- */
    pci_cfg_write(dev, 0, 0x04, pci_cfg_read(dev, 0, 0x04) | 0x6);
    uint64_t bar = pci_cfg_read(dev, 0, 0x10);
    if (bar & 1) return 0;                    /* I/O BAR: not expected */
    if (((bar >> 1) & 3) == 2)                /* 64-bit BAR: high half */
        bar |= (uint64_t)pci_cfg_read(dev, 0, 0x14) << 32;
    bar &= ~0xFULL;
    for (uint64_t i = 0; i < BAR_PAGES; i++)
        if (map_page(E1000_VIRT + (i << 12), bar + (i << 12),
                     PG_WRITE | PG_PCD | PG_PWT) != 0)
            return 0;

    /* -- reset, then re-mask interrupts (reset clears IMC) -- */
    wr(R_IMC, 0xFFFFFFFFu);
    wr(R_CTRL, rr(R_CTRL) | CTRL_RST);
    for (unsigned long i = 0; i < WAIT_BOUND; i++)
        if (!(rr(R_CTRL) & CTRL_RST)) break;
    if (rr(R_CTRL) & CTRL_RST) return 0;
    wr(R_IMC, 0xFFFFFFFFu);
    (void)rr(R_ICR);                          /* clear stale causes */
    wr(R_CTRL, (rr(R_CTRL) | CTRL_SLU | CTRL_ASDE));

    /* -- MAC: RAL/RAH first (QEMU preloads them), EEPROM fallback -- */
    uint32_t ral = rr(R_RAL0), rah = rr(R_RAH0);
    if ((rah & RAH_AV) && (ral || (rah & 0xFFFF))) {
        g.mac[0] = (uint8_t)ral;        g.mac[1] = (uint8_t)(ral >> 8);
        g.mac[2] = (uint8_t)(ral >> 16); g.mac[3] = (uint8_t)(ral >> 24);
        g.mac[4] = (uint8_t)rah;        g.mac[5] = (uint8_t)(rah >> 8);
    } else {
        uint16_t w0, w1, w2;
        if (!eeprom_word(0, &w0) || !eeprom_word(1, &w1) ||
            !eeprom_word(2, &w2))
            return 0;
        g.mac[0] = (uint8_t)w0; g.mac[1] = (uint8_t)(w0 >> 8);
        g.mac[2] = (uint8_t)w1; g.mac[3] = (uint8_t)(w1 >> 8);
        g.mac[4] = (uint8_t)w2; g.mac[5] = (uint8_t)(w2 >> 8);
    }
    /* program the receive-address filter from what we learned */
    wr(R_RAL0, (uint32_t)g.mac[0] | ((uint32_t)g.mac[1] << 8) |
               ((uint32_t)g.mac[2] << 16) | ((uint32_t)g.mac[3] << 24));
    wr(R_RAH0, (uint32_t)g.mac[4] | ((uint32_t)g.mac[5] << 8) | RAH_AV);
    for (int i = 0; i < 128; i++) wr(R_MTA + 4 * i, 0);

    /* -- rings + buffers from PMM pages (phys known by construction).
     * One page holds both rings: RX 32*16 = 512 B at +0, TX 8*16 =
     * 128 B at +512 (16-byte alignment and the 128-byte length rule
     * both hold). Frame buffers: 2 per page. -- */
    uint64_t ringp = pmm_alloc_page();
    if (!ringp) return 0;
    uint8_t *ringv = phys_to_virt(ringp);
    for (int i = 0; i < 4096; i++) ringv[i] = 0;
    g.rxr = (volatile rx_desc *)ringv;
    g.txr = (volatile tx_desc *)(ringv + 512);
    g.rxr_phys = ringp;
    g.txr_phys = ringp + 512;

    for (int i = 0; i < NRX; i += 2) {
        uint64_t p = pmm_alloc_page();
        if (!p) return 0;
        g.rxbuf[i]      = phys_to_virt(p);       g.rxbuf_phys[i]     = p;
        g.rxbuf[i + 1]  = (uint8_t *)phys_to_virt(p) + BUFSZ;
        g.rxbuf_phys[i + 1] = p + BUFSZ;
    }
    for (int i = 0; i < NTX; i += 2) {
        uint64_t p = pmm_alloc_page();
        if (!p) return 0;
        g.txbuf[i]      = phys_to_virt(p);       g.txbuf_phys[i]     = p;
        g.txbuf[i + 1]  = (uint8_t *)phys_to_virt(p) + BUFSZ;
        g.txbuf_phys[i + 1] = p + BUFSZ;
    }
    for (int i = 0; i < NRX; i++) {
        g.rxr[i].addr = g.rxbuf_phys[i];
        g.rxr[i].status = 0;
    }

    /* -- RX: head 0, tail NRX-1 (the hardware owns [head, tail]) -- */
    wr(R_RDBAL, (uint32_t)g.rxr_phys);
    wr(R_RDBAH, (uint32_t)(g.rxr_phys >> 32));
    wr(R_RDLEN, NRX * 16);
    wr(R_RDH, 0);
    wr(R_RDT, NRX - 1);
    g.rx_next = 0;
    wr(R_RCTL, RCTL_EN | RCTL_BAM | RCTL_SECRC);   /* BSIZE 00 = 2048 */

    /* -- TX -- */
    wr(R_TDBAL, (uint32_t)g.txr_phys);
    wr(R_TDBAH, (uint32_t)(g.txr_phys >> 32));
    wr(R_TDLEN, NTX * 16);
    wr(R_TDH, 0);
    wr(R_TDT, 0);
    g.tx_next = 0;
    wr(R_TIPG, TIPG_DEF);
    wr(R_TCTL, TCTL_EN | TCTL_PSP | TCTL_CT | TCTL_COLD);

    g.up = 1;
    return 1;
}

int e1000_send(const uint8_t *frame, int len) {
    if (!g.up || !frame || len <= 0 || len > E1000_FRAME_MAX) return 0;

    /* ring full? wait (bounded) for the head to advance */
    unsigned next = (g.tx_next + 1) % NTX;
    for (unsigned long i = 0; i < WAIT_BOUND; i++) {
        if (rr(R_TDH) != next) break;
    }
    if (rr(R_TDH) == next) return 0;

    uint8_t *b = g.txbuf[g.tx_next];
    ncopy(b, frame, (uint64_t)len);
    int padded = len;
    while (padded < E1000_FRAME_MIN) b[padded++] = 0;

    volatile tx_desc *d = &g.txr[g.tx_next];
    d->addr    = g.txbuf_phys[g.tx_next];
    d->length  = (uint16_t)padded;
    d->cso     = 0;
    d->cmd     = TXD_EOP | TXD_IFCS | TXD_RS;
    d->status  = 0;
    d->css     = 0;
    d->special = 0;

    g.tx_next = next;
    wr(R_TDT, next);                               /* hand it over */

    for (unsigned long i = 0; i < WAIT_BOUND; i++)
        if (d->status & TXD_DD) return 1;          /* sent (DMA'd out) */
    return 0;
}

int e1000_recv(uint8_t *buf, int cap) {
    if (!g.up || !buf || cap <= 0) return 0;
    volatile rx_desc *d = &g.rxr[g.rx_next];
    if (!(d->status & RXD_DD)) return 0;

    int len = d->length;
    if (len > cap) len = cap;
    if (len > 0) ncopy(buf, g.rxbuf[g.rx_next], (uint64_t)len);

    d->status = 0;                                 /* recycle */
    wr(R_RDT, g.rx_next);
    g.rx_next = (g.rx_next + 1) % NRX;
    return len;
}

int e1000_selftest_loopback(void) {
    if (!g.up) return 0;
    uint16_t bmcr;
    if (!phy_read(MII_BMCR, &bmcr)) return 0;
    if (!phy_write(MII_BMCR, bmcr | BMCR_LOOPBACK)) return 0;

    /* a self-addressed frame: passes the RA filter, never needs a
     * backend. 60 bytes, experimental ethertype 0x88B5. */
    uint8_t f[60];
    for (int i = 0; i < 6; i++) { f[i] = g.mac[i]; f[6 + i] = g.mac[i]; }
    f[12] = 0x88; f[13] = 0xB5;
    for (int i = 14; i < 60; i++) f[i] = (uint8_t)(i * 7);

    /* RETRIED sends, TIME-bounded receive waits, a >1 s total horizon.
     * QEMU arms a ONE-SECOND "receiver spin-up" timer on the RCTL
     * write (set_rctl: timer_mod +1000 ms); while it pends,
     * can_receive is false and a looped-back frame is DROPPED at
     * transmit time (qemu_receive_packet does not queue on refusal) —
     * a send inside that window loses the frame forever, and no
     * amount of RX polling brings it back (diagnosed: TDH advanced
     * per attempt, RDH pinned at 0, ICR never showed RXT0, and the
     * frame never reached the backend pcap). Each attempt drains
     * stale frames, sends, then waits ~250 ms of PIT ticks
     * (iteration-guarded so a dead PIT still FAILS, never hangs);
     * 8 attempts stretch past the spin-up on any tick rate. */
    int ok = 0;
    uint8_t back[BUFSZ];
    int n = 0;
    for (int attempt = 0; attempt < 8 && n == 0; attempt++) {
        while (e1000_recv(back, sizeof back) > 0) { }
        if (!e1000_send(f, 60)) break;
        ok = 1;
        uint64_t t0 = timer_ticks();
        for (unsigned long i = 0; i < 1000 * WAIT_BOUND && n == 0; i++) {
            if (timer_ticks() - t0 > timer_hz() / 4) break;
            n = e1000_recv(back, sizeof back);
        }
    }
    if (n != 60) ok = 0;
    if (ok)
        for (int i = 0; i < 60; i++)
            if (back[i] != f[i]) { ok = 0; break; }

    phy_write(MII_BMCR, bmcr);                     /* restore */
    return ok;
}
