/* IDT setup + C-side exception dispatch (milestone-1). */
#include "idt.h"
#include "serial.h"
#include "pic.h"
#include "timer.h"
#include "keyboard.h"
#include "vga.h"

struct idt_entry {
    uint16_t off_low;
    uint16_t selector;
    uint8_t  ist;
    uint8_t  type_attr;
    uint16_t off_mid;
    uint32_t off_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry g_idt[256];
static struct idt_ptr   g_idtp;

extern void *isr_stub_table[48];   /* from isr.S: 32 exceptions + 16 IRQs */

#define KERNEL_CS 0x08             /* GDT code selector (boot.S) */

static void set_gate(int n, void *handler) {
    uint64_t a = (uint64_t)handler;
    g_idt[n].off_low   = (uint16_t)(a & 0xFFFF);
    g_idt[n].selector  = KERNEL_CS;
    g_idt[n].ist       = 0;
    g_idt[n].type_attr = 0x8E;     /* present, DPL0, 64-bit interrupt gate */
    g_idt[n].off_mid   = (uint16_t)((a >> 16) & 0xFFFF);
    g_idt[n].off_high  = (uint32_t)((a >> 32) & 0xFFFFFFFF);
    g_idt[n].zero      = 0;
}

static volatile int g_exceptions = 0;
int idt_exceptions_handled(void) { return g_exceptions; }

static const char *exc_name(uint64_t v) {
    switch (v) {
    case 0:  return "#DE divide error";
    case 1:  return "#DB debug";
    case 2:  return "NMI";
    case 3:  return "#BP breakpoint";
    case 6:  return "#UD invalid opcode";
    case 8:  return "#DF double fault";
    case 13: return "#GP general protection";
    case 14: return "#PF page fault";
    default: return "exception";
    }
}

/* Called from isr_common with a pointer to the saved frame. */
void isr_dispatch(interrupt_frame *f) {
    /* Hardware IRQs (PIC remapped to 32-47 in M4): service and EOI, no
     * halt. Interrupts stay off for the handler (interrupt gate) and are
     * re-enabled by iretq restoring RFLAGS. */
    if (f->vector >= 32 && f->vector <= 47) {
        int irq = (int)(f->vector - 32);
        if (irq == 0)      timer_irq();
        else if (irq == 1) kbd_irq();
        pic_eoi(irq);
        return;
    }

    g_exceptions++;

    if (f->vector == 3) {
        /* #BP is a trap: saved RIP is the next instruction, so iretq
         * resumes cleanly. This is the M1 self-test's happy path. */
        serial_write("[idt] #BP breakpoint caught at rip=0x");
        serial_write_hex64(f->rip);
        serial_write(" -- resuming\n");
        return;
    }

    /* Anything else at this stage is unexpected and fatal: report the
     * full frame over serial and halt (no recovery path yet). */
    serial_write("[idt] FATAL exception ");
    serial_write_dec(f->vector);
    serial_write(" (");
    serial_write(exc_name(f->vector));
    serial_write(")\n       error=0x");
    serial_write_hex64(f->error_code);
    serial_write(" rip=0x");
    serial_write_hex64(f->rip);
    serial_write(" cs=0x");
    serial_write_hex64(f->cs);
    serial_write(" rflags=0x");
    serial_write_hex64(f->rflags);
    serial_write("\n");
    /* Also render to VGA (the T3200 has no serial) + 2 panic beeps (works
     * even with the screen dead). The full frame is in the RAM crash log. */
    vga_put_at(24, 0, "FATAL CPU EXCEPTION - see crash log (2 beeps)", 0x4F);
    speaker_beeps(2);
    for (;;) __asm__ volatile ("cli; hlt");
}

void idt_init(void) {
    for (int i = 0; i < 48; i++) set_gate(i, isr_stub_table[i]);
    g_idtp.limit = (uint16_t)(sizeof(g_idt) - 1);
    g_idtp.base  = (uint64_t)&g_idt[0];
    __asm__ volatile ("lidt %0" :: "m"(g_idtp));
}
