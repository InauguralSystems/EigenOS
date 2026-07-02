/* Boot-stack canary — the assert-inside half of the mn-repl heisenbug fix.
 *
 * Bare metal has no guard page: a stack overflow silently corrupts whatever
 * the linker placed below stack_bottom, and the resulting fault is delayed
 * and moves with every rebuild (that was the "#UD in .rodata" heisenbug —
 * root cause fixed upstream in EigenScript PR #361, ~12.7 KiB of compiler
 * stack per AST level). kernel_main paints this canary strip at
 * stack_bottom; the REPL checks it after every eval so any future overflow
 * panics loudly at the eval that caused it, naming the real failure instead
 * of leaving a layout-dependent time bomb. */
#ifndef STACKGUARD_H
#define STACKGUARD_H

#include <stdint.h>

#define STACK_CANARY        0xC0FFEE57ACC0DE01ULL
#define STACK_CANARY_QWORDS 8    /* 64-byte strip at stack_bottom */

extern char stack_bottom[];      /* boot.S */

static inline int stack_canary_ok(void) {
    for (int i = 0; i < STACK_CANARY_QWORDS; i++)
        if (((volatile uint64_t *)stack_bottom)[i] != STACK_CANARY) return 0;
    return 1;
}

#endif
