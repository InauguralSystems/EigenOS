/* EigenScript freestanding HAL (milestone-5).
 *
 * Binds the freestanding profile's residual libc symbols to EigenOS
 * services, sets up thread-local storage, and drives a demo eval. Built
 * with the RUNTIME cflags (-msse2, freestanding) — NOT the kernel core
 * flags — because (a) the variadic console functions must save xmm in
 * their prologue to handle %f/%g args, and (b) eigs_value_as_num returns
 * a double, which -mno-sse kernel code could not receive. The kernel
 * calls eigs_run_demo(), which returns a plain int. */
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>

#include "kheap.h"
#include "serial.h"
#include "timer.h"
#include "keyboard.h"
#include "stackguard.h"
#include "vga.h"
#include "eigs_embed.h"        /* from EigenScript/src, via -I in the Makefile */

/* Console fan-out: serial always (headless log); VGA once the REPL owns
 * the screen (so the boot status rows aren't clobbered mid-boot). */
static int g_vga_on;
static void con_write(const char *s) { serial_write(s); if (g_vga_on) vga_puts(s); }
static void con_putc(char c)         { serial_putc(c);  if (g_vga_on) vga_putc(c); }

/* ---- heap: malloc family -> kernel heap ---- */
void *malloc(size_t n)             { return kmalloc(n); }
void  free(void *p)                { kfree(p); }
void *calloc(size_t a, size_t b)   { return kcalloc(a, b); }
void *realloc(void *p, size_t n)   { return krealloc(p, n); }

/* ---- console: printf family -> COM1 serial ---- */
#undef putc
#undef stdout
#undef stderr
static FILE _out, _err;
FILE *stdout = &_out;
FILE *stderr = &_err;

static int hal_vprint(const char *fmt, va_list ap) {
    char buf[1024];
    int n = vsnprintf(buf, sizeof buf, fmt, ap);   /* mini_fmt does the FP */
    con_write(buf);
    return n;
}
int printf(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    int n = hal_vprint(fmt, ap);
    va_end(ap); return n;
}
int fprintf(FILE *s, const char *fmt, ...) {
    (void)s; va_list ap; va_start(ap, fmt);
    int n = hal_vprint(fmt, ap);
    va_end(ap); return n;
}
int fputs(const char *s, FILE *f)  { (void)f; con_write(s); return 0; }
int fputc(int c, FILE *f)          { (void)f; con_putc((char)c); return c; }
int putc(int c, FILE *f)           { (void)f; con_putc((char)c); return c; }
int fflush(FILE *f)                { (void)f; return 0; }

/* ---- clock: PIT ticks ---- */
int clock_gettime(clockid_t clk, struct timespec *ts) {
    (void)clk;
    uint64_t ticks = timer_ticks();
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    ts->tv_sec  = (time_t)(ticks / hz);
    ts->tv_nsec = (long)((ticks % hz) * (1000000000ULL / hz));
    return 0;
}
int usleep(unsigned int usec) {
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    uint64_t start = timer_ticks();
    uint64_t want = ((uint64_t)usec * hz) / 1000000ULL;
    while (timer_ticks() - start < want) __asm__ volatile ("hlt");
    return 0;
}

/* ---- halt ---- */
__attribute__((noreturn)) void exit(int code) {
    (void)code;
    serial_write("\n[hal] exit() -> halt\n");
    for (;;) __asm__ volatile ("cli; hlt");
}
__attribute__((noreturn)) void abort(void) {
    serial_write("\n[hal] abort() -> halt\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ---- threads: single-thread no-op stubs ----
 * A program that never spawns never calls cond_wait/create at runtime;
 * these exist only to satisfy the link. Mutex/cond are no-ops (one
 * thread, no contention); pthread_create refuses (spawn unsupported). */
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a) { (void)m; (void)a; return 0; }
int pthread_mutex_lock(pthread_mutex_t *m)    { (void)m; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *m)  { (void)m; return 0; }
int pthread_mutex_destroy(pthread_mutex_t *m) { (void)m; return 0; }
int pthread_cond_init(pthread_cond_t *c, const pthread_condattr_t *a) { (void)c; (void)a; return 0; }
int pthread_cond_wait(pthread_cond_t *c, pthread_mutex_t *m) { (void)c; (void)m; return 0; }
int pthread_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *t) { (void)c; (void)m; (void)t; return 0; }
int pthread_cond_signal(pthread_cond_t *c)    { (void)c; return 0; }
int pthread_cond_broadcast(pthread_cond_t *c) { (void)c; return 0; }
int pthread_cond_destroy(pthread_cond_t *c)   { (void)c; return 0; }
int pthread_condattr_init(pthread_condattr_t *a)    { (void)a; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *a) { (void)a; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *a, clockid_t c) { (void)a; (void)c; return 0; }
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg) {
    (void)t; (void)a; (void)fn; (void)arg; return 11 /* EAGAIN */;
}
int pthread_join(pthread_t t, void **r) { (void)t; (void)r; return 0; }

/* ---- TLS bring-up ----
 * The runtime uses __thread (the VM, dict cache, eigs_current). Set up a
 * single static TLS block (variant II): TP at the top, template below.
 * All our TLS is .tbss, so the block is just zeroed. IA32_FS_BASE = TP;
 * the compiler's local-exec accesses (%fs:var@tpoff, tpoff negative)
 * then resolve to block + var_offset. */
extern char __tdata_start[], __tdata_end[], __tls_end[];
static uint8_t g_tls_block[16384] __attribute__((aligned(64)));

static void wrmsr(uint32_t msr, uint64_t val) {
    __asm__ volatile ("wrmsr" :: "c"(msr), "a"((uint32_t)val), "d"((uint32_t)(val >> 32)));
}

void tls_init(void) {
    uint64_t tmpl   = (uint64_t)__tls_end - (uint64_t)__tdata_start;   /* memsz */
    uint64_t tdata  = (uint64_t)__tdata_end - (uint64_t)__tdata_start;
    uint64_t size   = (tmpl + 63) & ~63ULL;                            /* p_align=64 */
    for (uint64_t i = 0; i < size; i++) g_tls_block[i] = 0;
    for (uint64_t i = 0; i < tdata; i++)
        g_tls_block[i] = ((uint8_t *)__tdata_start)[i];
    uint64_t tp = (uint64_t)g_tls_block + size;
    *(uint64_t *)tp = tp;                 /* TCB self-pointer at [TP] */
    wrmsr(0xC0000100, tp);                /* IA32_FS_BASE */
}

/* ---- the demo: run EigenScript source on bare metal ----
 * Returns 1 if a numeric eval gives the expected answer. All the double
 * handling lives here (SSE side); the kernel only sees the int. */
/* M8: machine vitals as EigenScript values. Each host fn samples a live
 * kernel counter; observation happens through the language's own
 * mechanism — assign the sample to a binding in a loop and the binding's
 * observer slot accumulates the trajectory (`stable of hp`,
 * `diverging of hp`...). No new observer semantics needed: the vital IS
 * just a value. The `vitals` ROM-bundle module wraps these in
 * ratio/uptime helpers. */
#include "pmm.h"
static EigsValue *vital_heap_used(EigsValue *a)   { (void)a; return eigs_value_new_num((double)kheap_used_bytes()); }
static EigsValue *vital_heap_mapped(EigsValue *a) { (void)a; return eigs_value_new_num((double)kheap_mapped_bytes()); }
static EigsValue *vital_mem_free(EigsValue *a)    { (void)a; return eigs_value_new_num((double)pmm_free_pages()); }
static EigsValue *vital_mem_total(EigsValue *a)   { (void)a; return eigs_value_new_num((double)pmm_total_pages()); }
static EigsValue *vital_ticks(EigsValue *a)       { (void)a; return eigs_value_new_num((double)timer_ticks()); }
static EigsValue *vital_tick_hz(EigsValue *a)     { (void)a; return eigs_value_new_num((double)timer_hz()); }

static void register_vitals(void) {
    eigs_register_function("heap_used",       vital_heap_used);
    eigs_register_function("heap_mapped",     vital_heap_mapped);
    eigs_register_function("mem_free_pages",  vital_mem_free);
    eigs_register_function("mem_total_pages", vital_mem_total);
    eigs_register_function("ticks",           vital_ticks);
    eigs_register_function("tick_hz",         vital_tick_hz);
}

/* M7.5: the ROM bundle. build/bundle.c (generated by tools/mkbundle.py)
 * bakes the EigenScript stdlib + demos/ into the image; this provider
 * serves them through the runtime's source seam so `import` works with
 * no filesystem. Bundle strings live in .rodata — the seam copies
 * immediately, so lifetime is a non-issue. */
extern const char *eigs_bundle_lookup(const char *name);
extern int         eigs_bundle_count(void);
static const char *bundle_provider(const char *name, void *ud) {
    (void)ud;
    return eigs_bundle_lookup(name);
}

int eigs_run_demo(void) {
    tls_init();
    EigsState *st = eigs_open();
    if (!st) { serial_write("[m5] eigs_open FAILED\n"); return 0; }
    eigs_set_source_provider(bundle_provider, 0);
    serial_write("[m7.5] ROM bundle registered: ");
    serial_write_dec((uint64_t)eigs_bundle_count());
    serial_write(" modules\n");
    register_vitals();
    serial_write("[m8] machine vitals registered (heap/mem/ticks)\n");

    EigsValue *v = eigs_eval_string("6 * 7");
    if (!v || eigs_value_type(v) != EIGS_TYPE_NUM) {
        serial_write("[m5] eval '6 * 7' did not return a number\n");
        return 0;
    }
    int answer = (int)eigs_value_as_num(v);
    eigs_value_release(v);
    serial_write("[m5] eigs eval '6 * 7' = ");
    serial_write_dec((uint64_t)answer);
    serial_write((answer == 42) ? "  (expected 42) OK\n" : "  WRONG\n");
    if (answer != 42) return 0;

    /* Exercise the console path: a program that prints and computes. */
    serial_write("[m5] running a print program on bare metal:\n    ");
    EigsValue *r = eigs_eval_string(
        "n is 10\n"
        "sum is 0\n"
        "for i in range of n:\n"
        "    sum is sum + i\n"
        "print of (\"sum 0..9 = \" + (str of sum))\n");
    if (r) eigs_value_release(r);
    if (eigs_has_error()) {
        serial_write("[m5] print program error: ");
        serial_write(eigs_last_error_message());
        serial_write("\n");
        return 0;
    }

    /* The evals below used to be the heisenbug tripwires: a third eval, a
     * function `define`, and an observer loop faulted layout-dependently
     * (#UD in .rodata, moved with every rebuild). Root cause was NOT an
     * uninitialized read: the EigenScript compiler used ~12.7 KiB of C stack
     * per AST level (a by-value Compiler with an inline Local[512] array in
     * every nested-function frame), overflowing the old 64 KiB boot stack
     * during compile and silently trampling the .bss below stack_bottom — no
     * guard page on bare metal, so the corruption detonated evals later,
     * wherever the current layout put the victim global. Fixed upstream
     * (EigenScript PR #361, frame 12.7 KiB -> 1.4 KiB); the boot stack is
     * 1 MiB now and a canary strip at stack_bottom is checked after every
     * eval. These run at boot as the regression. */
    EigsValue *v3 = eigs_eval_string(
        "define square(k) as:\n"
        "    return k * k\n"
        "square of 12");
    int def_ok = v3 && eigs_value_type(v3) == EIGS_TYPE_NUM &&
                 (int)eigs_value_as_num(v3) == 144;
    if (v3) eigs_value_release(v3);
    serial_write(def_ok ? "[mn] eval 3 (define + call) = 144 OK\n"
                        : "[mn] eval 3 (define + call) FAILED\n");
    vga_put_at(17, 0, def_ok ? "MN: define + call = 144 OK"
                             : "MN: define + call FAILED", def_ok ? 0x0A : 0x4F);
    if (!def_ok) return 0;

    EigsValue *v4 = eigs_eval_string(
        "e is 5\n"
        "loop while not converged:\n"
        "    e is e * 0.5\n"
        "converged");
    int obs_ok = v4 && eigs_value_type(v4) == EIGS_TYPE_NUM &&
                 (int)eigs_value_as_num(v4) == 1;
    if (v4) eigs_value_release(v4);
    serial_write(obs_ok ? "[mn] eval 4 (observer loop) converged OK\n"
                        : "[mn] eval 4 (observer loop) FAILED\n");
    vga_put_at(18, 0, obs_ok ? "MN: observer loop converged OK"
                             : "MN: observer loop FAILED", obs_ok ? 0x0A : 0x4F);
    if (!obs_ok) return 0;

    /* M7.5: the machine has software — import from the ROM bundle. */
    EigsValue *v5 = eigs_eval_string(
        "import math\n"
        "math.abs of -7");
    int rom_ok = v5 && eigs_value_type(v5) == EIGS_TYPE_NUM &&
                 (int)eigs_value_as_num(v5) == 7;
    if (v5) eigs_value_release(v5);
    if (eigs_has_error()) { rom_ok = 0; eigs_clear_error(); }
    serial_write(rom_ok ? "[m7.5] import math (ROM bundle) -> abs(-7)=7 OK\n"
                        : "[m7.5] import math (ROM bundle) FAILED\n");
    vga_put_at(20, 0, rom_ok ? "M7.5: import math (ROM bundle) OK"
                             : "M7.5: import math (ROM bundle) FAILED",
               rom_ok ? 0x0A : 0x4F);
    if (!rom_ok) return 0;

    /* M8: the machine observes itself — sample a live kernel vital into
     * an ordinary binding; the observer accumulates the trajectory and
     * the predicates apply. A quiet heap must read `stable`. */
    EigsValue *v6 = eigs_eval_string(
        "import vitals\n"
        "hp is 0.0\n"
        "i is 0\n"
        "loop while i < 14:\n"
        "    hp is vitals.heap_pressure of []\n"
        "    i is i + 1\n"
        "(stable of hp) and (hp > 0) and (hp < 1)");
    int vit_ok = v6 && eigs_value_type(v6) == EIGS_TYPE_NUM &&
                 (int)eigs_value_as_num(v6) == 1;
    if (v6) eigs_value_release(v6);
    if (eigs_has_error()) { vit_ok = 0; eigs_clear_error(); }
    serial_write(vit_ok ? "[m8] stable of heap_pressure = 1 (machine observes itself) OK\n"
                        : "[m8] vitals observer self-test FAILED\n");
    vga_put_at(21, 0, vit_ok ? "M8: stable of heap_pressure OK"
                             : "M8: vitals observer FAILED",
               vit_ok ? 0x0A : 0x4F);
    if (!vit_ok) return 0;

    if (!stack_canary_ok()) {
        serial_write("[mn] PANIC: boot-stack canary trampled during demo\n");
        vga_put_at(19, 0, "MN: STACK CANARY TRAMPLED", 0x4F);
        return 0;
    }
    serial_write("[mn] stack canary intact after 4 evals OK\n");
    vga_put_at(19, 0, "MN: stack canary intact OK", 0x0A);
    return 1;
}

/* ---- MN: interactive EigenScript REPL on bare metal ----
 * PS/2 keyboard (kbd_getchar) in, serial + VGA out. One open runtime, so
 * state persists across lines. Each Enter evals the line; the last
 * expression's value is echoed (numbers/strings), errors reported. `print`
 * output already flows through the console fan-out. Single lines eval on
 * Enter; a line ending in ':' opens a block that accumulates until a blank
 * line submits it (M6) — so `define`, `for`, and observer loops can be
 * typed interactively. */
static void repl_show_value(EigsValue *v) {
    if (!v) return;
    switch (eigs_value_type(v)) {
    case EIGS_TYPE_NUM: {
        char b[64];
        snprintf(b, sizeof b, "%g\n", eigs_value_as_num(v));
        con_write(b);
        break;
    }
    case EIGS_TYPE_STR:
        con_write(eigs_value_as_string(v));
        con_putc('\n');
        break;
    case EIGS_TYPE_NULL:
        break;                                    /* statements print nothing */
    default:
        con_write("<value>\n");
        break;
    }
}

/* Evaluate one submitted program (a single line or an accumulated block),
 * echo the result/error, then assert the boot-stack canary — an overflow
 * past stack_bottom must fail HERE, loudly, not as a delayed
 * layout-dependent fault. */
static void repl_eval(const char *src) {
    EigsValue *v = eigs_eval_string(src);
    if (eigs_has_error()) {
        con_write("error: ");
        con_write(eigs_last_error_message());
        con_putc('\n');
        eigs_clear_error();
    } else {
        repl_show_value(v);
    }
    if (v) eigs_value_release(v);
    if (!stack_canary_ok()) {
        con_write("PANIC: boot-stack overflow (canary trampled by that "
                  "eval). Halting.\n");
        speaker_beeps(3);            /* 3 beeps = canary (2 = CPU exception) */
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

void eigs_repl(void) {
    g_vga_on = 1;
    vga_clear();
    con_write("EigenScript REPL on EigenOS -- Enter evals. A line ending in ':'\n");
    con_write("opens a block: keep typing lines, a blank line runs it.\n");
    con_write("eigs> ");

    char line[512];
    static char prog[4096];       /* accumulated block (M6 multi-line) */
    int len  = 0;                 /* current line fill */
    int plen = 0;                 /* accumulated block fill */
    int cont = 0;                 /* 1 = inside a ':'-opened block */
    for (;;) {
        int c = kbd_getchar();
        if (c < 0) { __asm__ volatile ("hlt"); continue; }   /* wait for a key */

        if (c == '\n') {
            con_putc('\n');
            line[len] = 0;
            /* Does this line open a block? Trailing ':' (spaces ignored). */
            int e = len;
            while (e > 0 && line[e - 1] == ' ') e--;
            int opens = (e > 0 && line[e - 1] == ':');

            if (!cont) {
                if (len == 0) { con_write("eigs> "); continue; }
                if (opens) {                      /* start accumulating */
                    plen = 0;
                    for (int i = 0; i < len; i++) prog[plen++] = line[i];
                    prog[plen++] = '\n';
                    cont = 1;
                    con_write("....> ");
                } else {                          /* single line: eval now */
                    repl_eval(line);
                    con_write("eigs> ");
                }
            } else {
                if (len == 0) {                   /* blank line: run block */
                    prog[plen] = 0;
                    cont = 0;
                    repl_eval(prog);
                    con_write("eigs> ");
                } else if (plen + len + 2 > (int)sizeof(prog)) {
                    con_write("error: block too large, discarded\n");
                    cont = 0;
                    con_write("eigs> ");
                } else {                          /* keep accumulating */
                    for (int i = 0; i < len; i++) prog[plen++] = line[i];
                    prog[plen++] = '\n';
                    con_write("....> ");
                }
            }
            len = 0;
        } else if (c == '\b') {
            if (len > 0) { len--; con_write("\b \b"); }
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
            con_putc((char)c);                    /* echo */
        }
    }
}
