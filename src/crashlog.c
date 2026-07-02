/* Persistent-RAM boot/crash log — see crashlog.h for the machine
 * contract. Kernel-flags code (no SSE): crashlog_putc is called from
 * serial_putc, including from the exception dispatcher in IRQ context. */
#include "crashlog.h"

static crashlog_hdr *H;        /* NULL until crashlog_init */
static char         *DATA;

/* memcmp/memcpy-free on purpose: this runs before anything else and must
 * not pull in mini-libc (SSE-compiled) symbols. */
void crashlog_init(void) {
    crashlog_hdr *h = (crashlog_hdr *)CRASHLOG_PHYS;
    static const char magic[8] = CRASHLOG_MAGIC;
    int had_magic = 1;
    for (int i = 0; i < 8; i++)
        if (h->magic[i] != magic[i]) { had_magic = 0; break; }

    uint32_t boots = had_magic ? h->boot_count : 0;
    for (int i = 0; i < 8; i++) h->magic[i] = magic[i];
    h->cap        = (uint32_t)(CRASHLOG_SIZE - sizeof(crashlog_hdr));
    h->head       = 0;
    h->wrapped    = 0;
    h->boot_count = boots + 1;

    H    = h;
    DATA = (char *)CRASHLOG_PHYS + sizeof(crashlog_hdr);
}

void crashlog_putc(char c) {
    if (!H) return;
    DATA[H->head] = c;
    H->head++;
    if (H->head >= H->cap) { H->head = 0; H->wrapped = 1; }
}
