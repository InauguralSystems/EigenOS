/* EigenOS kernel entry (64-bit long mode).
 *
 * Milestone-0: prove the build + boot + long-mode pipeline by writing to VGA
 * text memory (0xB8000, identity-mapped by the boot stub) and halting. No libc,
 * no runtime — this is the floor we build the kernel on, toward running the
 * EigenScript runtime freestanding. */
#include <stdint.h>

#define VGA       ((volatile uint16_t *)0xB8000)
#define VGA_COLS  80
#define VGA_ROWS  25

/* attr<<8 | char : light-grey-on-black (0x07), white-on-black (0x0F) */
static inline uint16_t cell(char c, uint8_t attr) {
    return (uint16_t)c | ((uint16_t)attr << 8);
}

static void clear(uint8_t attr) {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++)
        VGA[i] = cell(' ', attr);
}

static void put_at(int row, int col, const char *s, uint8_t attr) {
    int i = row * VGA_COLS + col;
    for (; *s; s++, i++)
        VGA[i] = cell(*s, attr);
}

void kernel_main(void) {
    clear(0x07);
    put_at(0, 0, "EigenOS", 0x0F);
    put_at(0, 8, "// bare metal, x86-64 long mode", 0x07);
    put_at(2, 0, "milestone-0: boot + long mode + VGA. next: IDT, then the runtime.", 0x08);
    for (;;)
        __asm__ volatile ("hlt");
}
