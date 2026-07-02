/* COM1 16550 UART driver (milestone-1). */
#include "serial.h"
#include "io.h"
#include "crashlog.h"

#define COM1 0x3F8

void serial_init(void) {
    outb(COM1 + 1, 0x00);   /* disable UART interrupts            */
    outb(COM1 + 3, 0x80);   /* DLAB on: next two writes = divisor */
    outb(COM1 + 0, 0x01);   /* divisor 1 -> 115200 baud (low)     */
    outb(COM1 + 1, 0x00);   /* divisor high                       */
    outb(COM1 + 3, 0x03);   /* DLAB off, 8 bits, no parity, 1 stop*/
    outb(COM1 + 2, 0xC7);   /* enable+clear FIFO, 14-byte trigger */
    outb(COM1 + 4, 0x0B);   /* RTS/DSR set, OUT2 (IRQ line) on    */
}

/* Loopback self-test: route TX->RX in the UART, send a byte, read it
 * back. Proves the port answers without needing a listener on the wire.
 * Saves/restores the modem-control register so normal TX still works. */
int serial_selftest(void) {
    uint8_t saved = inb(COM1 + 4);
    outb(COM1 + 4, 0x1E);              /* loopback + OUT1/OUT2/RTS */
    outb(COM1 + 0, 0xAE);
    int ok = 0;
    for (int i = 0; i < 100000; i++) {
        if (inb(COM1 + 5) & 0x01) {    /* LSR: data ready */
            ok = (inb(COM1 + 0) == 0xAE);
            break;
        }
    }
    outb(COM1 + 4, saved);
    return ok;
}

static int transmit_empty(void) { return inb(COM1 + 5) & 0x20; }

void serial_putc(char c) {
    crashlog_putc(c);            /* mirror every log byte to persistent RAM */
    if (c == '\n') {
        while (!transmit_empty()) { }
        outb(COM1, '\r');
    }
    while (!transmit_empty()) { }
    outb(COM1, (uint8_t)c);
}

void serial_write(const char *s) {
    for (; *s; s++) serial_putc(*s);
}

void serial_write_hex64(uint64_t v) {
    static const char d[] = "0123456789abcdef";
    for (int sh = 60; sh >= 0; sh -= 4) serial_putc(d[(v >> sh) & 0xF]);
}

void serial_write_dec(uint64_t v) {
    char t[21];
    int n = 0;
    if (v == 0) { serial_putc('0'); return; }
    while (v) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) serial_putc(t[--n]);
}
