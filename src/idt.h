/* Interrupt Descriptor Table + CPU exception handling (milestone-1).
 * The GDT is already established by M0's boot stub (null/code/data);
 * M1 adds the IDT. TSS/IST are deferred until a privilege boundary
 * (ring 3) actually needs a stack switch. */
#ifndef EIGENOS_IDT_H
#define EIGENOS_IDT_H

#include <stdint.h>

/* Layout MUST match the push order in isr.S (isr_common + the CPU frame).
 * Lowest address first = last pushed. */
typedef struct {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;          /* pushed by the stubs */
    uint64_t rip, cs, rflags, rsp, ss;    /* pushed by the CPU    */
} interrupt_frame;

void idt_init(void);
int  idt_exceptions_handled(void);        /* count, for the boot self-test */

#endif /* EIGENOS_IDT_H */
