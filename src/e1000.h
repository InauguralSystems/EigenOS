/* M13: Intel 8254x (e1000) NIC driver — raw ethernet frames.
 *
 * Mechanism only, the ATA/kbd split: this file owns PCI discovery, the
 * MMIO register block, and the RX/TX descriptor rings (the DMA banking
 * the language cannot do); frame POLICY (ARP, echo, everything above
 * the frame boundary) lives in .eigs over the hal.c net_* seam.
 * Polled, no IRQ path — same rationale as ATA (bounded behavior beats
 * throughput under a replayable machine, and the M12 claim seam is
 * byte-oriented 8042 territory). Every wait is iteration-bounded: an
 * absent or dead NIC produces a FAILED/absent verdict, never a hang. */
#ifndef EIGENOS_E1000_H
#define EIGENOS_E1000_H

#include <stdint.h>

#define E1000_FRAME_MAX 1518          /* no jumbo: LPE stays off */
#define E1000_FRAME_MIN 60            /* short TX frames are zero-padded */

/* Probe PCI bus 0 for a supported 8254x, map BAR0, reset, bring the
 * rings up. Returns 1 when the NIC is up, 0 when absent/failed
 * (distinguish with e1000_present). Requires paging + PMM. */
int e1000_init(void);

int            e1000_present(void);   /* probe matched + bring-up done */
const uint8_t *e1000_mac(void);       /* 6 bytes, valid after init */

/* Transmit one frame (bounded wait for a free descriptor and for the
 * DD writeback). len is clamped to E1000_FRAME_MAX; short frames are
 * padded to E1000_FRAME_MIN. Returns 1 sent, 0 refused/timeout. */
int e1000_send(const uint8_t *frame, int len);

/* Non-blocking receive: copies the next pending frame into buf (at
 * most cap bytes; frames are <= E1000_FRAME_MAX + 4 with SECRC on,
 * so 2048 always fits) and returns its length, or 0 when the RX ring
 * has nothing. Consumes the descriptor either way it returns > 0. */
int e1000_recv(uint8_t *buf, int cap);

/* Boot self-test: PHY loopback (MII BMCR bit 14) a 60-byte frame
 * addressed to ourselves and require the exact bytes back on the RX
 * ring — the 8042-0xD2 analog: a full TX->RX round trip with no
 * external backend. Restores the PHY afterwards. 1 = byte-exact. */
int e1000_selftest_loopback(void);

#endif /* EIGENOS_E1000_H */
