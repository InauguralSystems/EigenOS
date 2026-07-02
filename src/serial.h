/* COM1 (16550 UART) serial console — the kernel's headless logging seam.
 * This is what becomes console_write's backend when EigenScript runs
 * freestanding (M5); until then it's how a display-less boot is observed
 * (QEMU `-serial stdio`, or `make serial`). */
#ifndef EIGENOS_SERIAL_H
#define EIGENOS_SERIAL_H

#include <stdint.h>

void serial_init(void);
int  serial_selftest(void);        /* UART loopback: 1 = ok, 0 = absent/broken */
void serial_putc(char c);          /* '\n' is expanded to CRLF */
void serial_write(const char *s);
void serial_write_hex64(uint64_t v);
void serial_write_dec(uint64_t v);

#endif /* EIGENOS_SERIAL_H */
