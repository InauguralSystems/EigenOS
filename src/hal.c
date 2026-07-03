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
#include <string.h>            /* declarations; the link resolves to mini_libc */
#include <time.h>
#include <pthread.h>

#include "kheap.h"
#include "serial.h"
#include "timer.h"
#include "keyboard.h"
#include "stackguard.h"
#include "vga.h"
#include "fb.h"
#include "mouse.h"
#include "ata.h"
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

/* M11: vitals are NONDET machine inputs — each participates in the
 * trace tape with the take/record contract (upstream PR #365), so a
 * journaled session replays with the RECORDED machine readings instead
 * of live ones. tick_hz / mem_total_pages are boot constants and stay
 * plain. */
static EigsValue *vital_nondet(const char *name, double live) {
    EigsValue *v;
    if (eigs_replay_take(name, &v)) return v;
    v = eigs_value_new_num(live);
    eigs_trace_record_nondet(name, v);
    return v;
}
static EigsValue *vital_heap_used(EigsValue *a)   { (void)a; return vital_nondet("heap_used", (double)kheap_used_bytes()); }
static EigsValue *vital_heap_mapped(EigsValue *a) { (void)a; return vital_nondet("heap_mapped", (double)kheap_mapped_bytes()); }
static EigsValue *vital_mem_free(EigsValue *a)    { (void)a; return vital_nondet("mem_free_pages", (double)pmm_free_pages()); }
static EigsValue *vital_mem_total(EigsValue *a)   { (void)a; return eigs_value_new_num((double)pmm_total_pages()); }
static EigsValue *vital_ticks(EigsValue *a)       { (void)a; return vital_nondet("ticks", (double)timer_ticks()); }
static EigsValue *vital_tick_hz(EigsValue *a)     { (void)a; return eigs_value_new_num((double)timer_hz()); }

/* helpers defined further down (M9.5/M10 sections) */
static double list_num(EigsValue *l, int i);
static int arg_list_min(EigsValue *a, int n);
static int path_arg(EigsValue *v, char *dst, int cap);

/* ---- M11: machine history — state_at against the MACHINE ----
 * The language's `state_at of L` is line-keyed (a program's assignment
 * timeline); machine time is the other axis (tidelog's F-TEMPORAL-1
 * drew exactly this boundary). So machine history is TIME-keyed: a
 * sampler ring records the vitals about once a second from the REPL's
 * idle loop and the gfx frame path (cooperative machine, cooperative
 * sampler — a long eval samples nothing), boot to now, bounded.
 * Queries:
 *   vital_at of ["heap_used", t]     reading at the latest sample <= t
 *                                    (ticks); null before history
 *   vital_series of ["heap_used", n] buffer of the last n samples
 *   history_span of []               [oldest_tick, newest_tick, count]
 * All three are tape-participating nondet — a journaled session's
 * history queries replay exactly (series buffers ride the N records'
 * b[...] form). */
#define VH_CAP 4096                     /* ~68 min at 1 Hz */
static struct {
    uint64_t tick, heap_used, heap_mapped, mem_free;
} g_vh[VH_CAP];
static unsigned g_vh_head, g_vh_count;  /* head = next write slot */
static uint64_t g_vh_last_tick;

static void vitals_sample_maybe(void) {
    uint64_t now = timer_ticks();
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    if (g_vh_count && now - g_vh_last_tick < hz) return;   /* ~1 Hz */
    g_vh_last_tick = now;
    g_vh[g_vh_head].tick        = now;
    g_vh[g_vh_head].heap_used   = kheap_used_bytes();
    g_vh[g_vh_head].heap_mapped = kheap_mapped_bytes();
    g_vh[g_vh_head].mem_free    = pmm_free_pages();
    g_vh_head = (g_vh_head + 1) % VH_CAP;
    if (g_vh_count < VH_CAP) g_vh_count++;
}

/* sample i (0 = oldest) -> the named field; -1 on unknown name */
static double vh_field(unsigned i, const char *name) {
    unsigned idx = (g_vh_head + VH_CAP - g_vh_count + i) % VH_CAP;
    if (strcmp(name, "heap_used") == 0)      return (double)g_vh[idx].heap_used;
    if (strcmp(name, "heap_mapped") == 0)    return (double)g_vh[idx].heap_mapped;
    if (strcmp(name, "mem_free_pages") == 0) return (double)g_vh[idx].mem_free;
    if (strcmp(name, "ticks") == 0)          return (double)g_vh[idx].tick;
    return -1.0;
}

static uint64_t vh_tick(unsigned i) {
    return g_vh[(g_vh_head + VH_CAP - g_vh_count + i) % VH_CAP].tick;
}

static EigsValue *b_vital_at(EigsValue *a) {   /* [name, tick] -> num|null */
    EigsValue *v;
    if (eigs_replay_take("vital_at", &v)) return v;
    char name[32];
    v = eigs_value_new_null();
    if (arg_list_min(a, 2)) {
        EigsValue *nv = eigs_value_list_get(a, 0);
        int ok = nv && path_arg(nv, name, sizeof name);
        if (nv) eigs_value_release(nv);
        double t = list_num(a, 1);
        if (ok && g_vh_count) {
            /* latest sample at or before t; none -> null (before history) */
            int found = -1;
            for (unsigned i = 0; i < g_vh_count; i++) {
                if ((double)vh_tick(i) > t) break;
                found = (int)i;
            }
            if (found >= 0) {
                double x = vh_field((unsigned)found, name);
                if (x >= 0) {
                    eigs_value_release(v);
                    v = eigs_value_new_num(x);
                }
            }
        }
    }
    eigs_trace_record_nondet("vital_at", v);
    return v;
}

static EigsValue *b_vital_series(EigsValue *a) { /* [name, n] -> buffer */
    EigsValue *v;
    if (eigs_replay_take("vital_series", &v)) return v;
    char name[32];
    int n = 0, ok = 0;
    if (arg_list_min(a, 2)) {
        EigsValue *nv = eigs_value_list_get(a, 0);
        ok = nv && path_arg(nv, name, sizeof name);
        if (nv) eigs_value_release(nv);
        n = (int)list_num(a, 1);
    }
    if (!ok || n <= 0) n = 0;
    if ((unsigned)n > g_vh_count) n = (int)g_vh_count;
    v = eigs_value_new_buffer(n);
    for (int i = 0; i < n; i++)                 /* oldest->newest window */
        eigs_value_buffer_set(v, i,
            vh_field(g_vh_count - (unsigned)n + (unsigned)i, name));
    eigs_trace_record_nondet("vital_series", v);
    return v;
}

static EigsValue *b_history_span(EigsValue *a) { /* -> [oldest, newest, count] */
    (void)a;
    EigsValue *v;
    if (eigs_replay_take("history_span", &v)) return v;
    v = eigs_value_new_list(3);
    EigsValue *e;
    e = eigs_value_new_num(g_vh_count ? (double)vh_tick(0) : 0);
    eigs_value_list_append(v, e); eigs_value_release(e);
    e = eigs_value_new_num(g_vh_count ? (double)vh_tick(g_vh_count - 1) : 0);
    eigs_value_list_append(v, e); eigs_value_release(e);
    e = eigs_value_new_num((double)g_vh_count);
    eigs_value_list_append(v, e); eigs_value_release(e);
    eigs_trace_record_nondet("history_span", v);
    return v;
}

static void register_vitals(void) {
    eigs_register_function("heap_used",       vital_heap_used);
    eigs_register_function("heap_mapped",     vital_heap_mapped);
    eigs_register_function("mem_free_pages",  vital_mem_free);
    eigs_register_function("mem_total_pages", vital_mem_total);
    eigs_register_function("ticks",           vital_ticks);
    eigs_register_function("tick_hz",         vital_tick_hz);
    eigs_register_function("vital_at",        b_vital_at);
    eigs_register_function("vital_series",    b_vital_series);
    eigs_register_function("history_span",    b_history_span);
}

/* M9: cooperative multitasking — a task IS an EigsState. The upstream
 * eigs_thread_switch seam parks/activates per-state attachments on this
 * one CPU thread; a task "step" = one eval on the task's state (the
 * cooperative unit — evals run to completion). The SCHEDULER is script:
 * these builtins are the mechanism, demos/sched.eigs + the observer
 * predicates are the policy (a task whose `progress` reads `stable` is
 * stalled — deschedule it). Cross-state rule: only NUMBERS cross a
 * switch (copied); Value pointers never do. */
#define MAX_TASKS 8
#define TASK_KEYQ 16
static struct {
    EigsState *st;
    int alive;
    /* MD: the window seam — a viewport this task's gfx calls are
     * translated+clipped into, and a WM-fed key queue (numbers only
     * cross states, so keys travel as codes). */
    int has_view, vx, vy, vw, vh;
    int keyq[TASK_KEYQ];
    unsigned kq_head, kq_tail;
    int wheel;                  /* WM-fed scroll accumulator (deltas add) */
} g_tasks[MAX_TASKS];
static EigsState *g_primary;   /* the REPL/scheduler state */
static int g_active_task = -1; /* task whose state is mid-eval; -1 = primary */

extern const char *eigs_bundle_lookup(const char *name);

static void register_task_side(void);   /* gfx + win_* into a task state */

static EigsValue *b_task_spawn(EigsValue *arg) {
    char name[64];
    if (!arg || eigs_value_type(arg) != EIGS_TYPE_STR) return eigs_value_new_num(-1);
    { const char *n = eigs_value_as_string(arg);
      int i = 0;
      for (; n && n[i] && i < 63; i++) name[i] = n[i];
      name[i] = 0; }
    const char *src = eigs_bundle_lookup(name);
    if (!src) return eigs_value_new_num(-1);
    int slot = -1;
    for (int i = 0; i < MAX_TASKS; i++)
        if (!g_tasks[i].alive) { slot = i; break; }
    if (slot < 0) return eigs_value_new_num(-1);
    g_tasks[slot].has_view = 0;
    g_tasks[slot].kq_head = g_tasks[slot].kq_tail = 0;
    g_tasks[slot].wheel = 0;

    EigsState *st = eigs_state_new();
    if (!st || !eigs_thread_switch(st)) return eigs_value_new_num(-1);
    eigs_state_init_runtime(st);
    register_task_side();                      /* draw/win seams (MD) */
    g_active_task = slot;                      /* no viewport yet: a drawing
                                                  module top-level clips away */
    EigsValue *r = eigs_eval_string(src);      /* run the task program */
    if (r) eigs_value_release(r);
    g_active_task = -1;
    int failed = eigs_has_error();
    if (failed) { eigs_clear_error(); eigs_close(st); }
    eigs_thread_switch(g_primary);
    if (failed) return eigs_value_new_num(-1);
    g_tasks[slot].st = st;
    g_tasks[slot].alive = 1;
    return eigs_value_new_num((double)slot);
}

static int task_id_of(EigsValue *v) {
    if (!v || eigs_value_type(v) != EIGS_TYPE_NUM) return -1;
    int id = (int)eigs_value_as_num(v);
    if (id < 0 || id >= MAX_TASKS || !g_tasks[id].alive) return -1;
    return id;
}

static EigsValue *b_task_step(EigsValue *arg) {
    int id = task_id_of(arg);
    if (id < 0) return eigs_value_new_num(-1);
    eigs_thread_switch(g_tasks[id].st);
    g_active_task = id;                         /* gfx lands in its viewport */
    EigsValue *r = eigs_eval_string("step of []");
    double out = (r && eigs_value_type(r) == EIGS_TYPE_NUM)
                     ? eigs_value_as_num(r) : 0.0;
    if (r) eigs_value_release(r);               /* released on the TASK state */
    g_active_task = -1;
    int failed = eigs_has_error();
    if (failed) eigs_clear_error();
    eigs_thread_switch(g_primary);
    if (failed) { g_tasks[id].alive = 0; return eigs_value_new_num(-1); }
    return eigs_value_new_num(out);
}

static EigsValue *b_task_read(EigsValue *arg) {  /* [id, "name"] -> num */
    if (!arg || eigs_value_type(arg) != EIGS_TYPE_LIST ||
        eigs_value_list_len(arg) != 2)
        return eigs_value_new_num(0);
    EigsValue *idv = eigs_value_list_get(arg, 0);
    EigsValue *nv  = eigs_value_list_get(arg, 1);
    char name[64]; name[0] = 0;
    if (nv && eigs_value_type(nv) == EIGS_TYPE_STR) {
        const char *n = eigs_value_as_string(nv);
        int i = 0;
        for (; n && n[i] && i < 63; i++) name[i] = n[i];
        name[i] = 0;
    }
    int id = task_id_of(idv);
    if (idv) eigs_value_release(idv);
    if (nv)  eigs_value_release(nv);
    if (id < 0 || !name[0]) return eigs_value_new_num(0);
    eigs_thread_switch(g_tasks[id].st);
    EigsValue *g = eigs_get_global(name);
    double out = (g && eigs_value_type(g) == EIGS_TYPE_NUM)
                     ? eigs_value_as_num(g) : 0.0;
    if (g) eigs_value_release(g);               /* released on the TASK state */
    eigs_thread_switch(g_primary);
    return eigs_value_new_num(out);
}

static EigsValue *b_task_alive(EigsValue *arg) {
    return eigs_value_new_num(task_id_of(arg) >= 0 ? 1 : 0);
}

static EigsValue *b_task_kill(EigsValue *arg) {
    int id = task_id_of(arg);
    if (id < 0) return eigs_value_new_num(0);
    eigs_thread_switch(g_tasks[id].st);
    eigs_close(g_tasks[id].st);                 /* leaves the thread detached */
    eigs_thread_switch(g_primary);
    g_tasks[id].st = 0;
    g_tasks[id].alive = 0;
    return eigs_value_new_num(1);
}

static void register_tasks(void) {
    eigs_register_function("task_spawn", b_task_spawn);
    eigs_register_function("task_step",  b_task_step);
    eigs_register_function("task_read",  b_task_read);
    eigs_register_function("task_alive", b_task_alive);
    eigs_register_function("task_kill",  b_task_kill);
}

/* ---- M10: the block device, EigenScript-visible ----
 * Mechanism in ata.c; POLICY lives up here and in script. Sectors
 * travel as BUFFERS of 512 byte values (upstream PR #364 added the
 * buffer embed API for exactly this seam) — NUL-safe, no copy at the
 * value boundary, and the same shape tidelog's store I/O speaks.
 * blk_write REFUSES until blk_unlock has been called once this boot:
 * on the T3200 the primary disk carries the Linux install, so a
 * fat-fingered REPL line must not be able to write. Nothing in the
 * kernel ever unlocks by itself; the store's mount code does, after
 * verifying the tidelog magic. */
static double list_num(EigsValue *l, int i);      /* defined below (M9.5) */
static int arg_list_min(EigsValue *a, int n);
static int g_blk_unlocked;

static EigsValue *b_blk_present(EigsValue *a) {
    (void)a;
    return eigs_value_new_num(ata_present() ? 1 : 0);
}

static EigsValue *b_blk_sectors(EigsValue *a) {
    (void)a;
    return eigs_value_new_num((double)ata_sectors());
}

static EigsValue *b_blk_model(EigsValue *a) {
    (void)a;
    return eigs_value_new_string(ata_present() ? ata_model() : "");
}

static EigsValue *b_blk_unlock(EigsValue *a) {
    (void)a;
    g_blk_unlocked = 1;
    serial_write("[m10] blk: WRITES UNLOCKED for this boot\n");
    return eigs_value_new_num(1);
}

static EigsValue *b_blk_read(EigsValue *a) {   /* lba -> buffer[512] | null */
    if (!a || eigs_value_type(a) != EIGS_TYPE_NUM) return eigs_value_new_null();
    double d = eigs_value_as_num(a);
    if (d < 0) return eigs_value_new_null();
    uint8_t buf[512];
    if (!ata_read((uint64_t)d, buf)) return eigs_value_new_null();
    EigsValue *bv = eigs_value_new_buffer(512);
    for (int i = 0; i < 512; i++)
        eigs_value_buffer_set(bv, i, (double)buf[i]);
    return bv;
}

static EigsValue *b_blk_write(EigsValue *a) {  /* [lba, buffer[512]] -> 1|0 */
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    if (!g_blk_unlocked) {
        serial_write("[m10] blk_write REFUSED: writes locked (blk_unlock first)\n");
        return eigs_value_new_num(0);
    }
    double lba = list_num(a, 0);
    if (lba < 0) return eigs_value_new_num(0);
    EigsValue *data = eigs_value_list_get(a, 1);
    if (!data || eigs_value_type(data) != EIGS_TYPE_BUFFER ||
        eigs_value_buffer_len(data) != 512) {
        if (data) eigs_value_release(data);
        return eigs_value_new_num(0);
    }
    uint8_t buf[512];
    for (int i = 0; i < 512; i++)
        buf[i] = (uint8_t)eigs_value_buffer_get(data, i);
    eigs_value_release(data);
    return eigs_value_new_num(ata_write((uint64_t)lba, buf) ? 1 : 0);
}

static EigsValue *b_blk_flush(EigsValue *a) {
    (void)a;
    return eigs_value_new_num(ata_flush() ? 1 : 0);
}

static void register_blk(void) {
    eigs_register_function("blk_present", b_blk_present);
    eigs_register_function("blk_sectors", b_blk_sectors);
    eigs_register_function("blk_model",   b_blk_model);
    eigs_register_function("blk_read",    b_blk_read);
    eigs_register_function("blk_write",   b_blk_write);
    eigs_register_function("blk_unlock",  b_blk_unlock);
    eigs_register_function("blk_flush",   b_blk_flush);
}

/* ---- M10: the fs surface, un-carved ----
 * The freestanding profile carves read_text/write_text/read_bytes_buf/
 * write_bytes/file_exists/rename/remove_file out of the runtime; here
 * EigenOS provides those names itself through the embed API — the
 * vitals/tasks/gfx pattern. Each builtin delegates to the FS DAEMON
 * STATE (g_fs_state), a dedicated EigsState running the byte-unmodified
 * demos/regionfs.eigs; the delegation is task_eval's proven shape (park
 * the calling state mid-builtin, switch, eval, switch back — the fsd is
 * always idle between calls, so there is no VM nesting on any state).
 * Bytes cross the boundary BY COPY through a C staging buffer, per the
 * cross-state rule. One state owns the disk: any number of stores
 * (import tidefs) on any states layer over these builtins, and the
 * single mount authority is the fsd.
 *
 * Mount policy: mount-by-magic IS the authorization — a disk carrying
 * a valid regionfs superblock was deliberately formatted, so the boot
 * probe may mount (and thereby unlock writes) on its own; a foreign
 * disk (the T3200's Linux MBR) refuses the mount and writes stay
 * locked. fs_format remains the one manual act: virgin disks need an
 * operator's blk_unlock first. */
static EigsState *g_fs_state;
static int g_fs_mounted;

static EigsState *cur_state(void) {
    return g_active_task >= 0 ? g_tasks[g_active_task].st : g_primary;
}

/* All fsd evals go through this pair. g_fsd_busy gates the M11 journal
 * sink off while the fs daemon runs: its regionfs internals are
 * MACHINERY, not session activity — without the gate every flush
 * records its own per-byte write loops into the ring it is flushing
 * (thousands of A records per flush; each flush slower than the last). */
static int g_fsd_busy;

static EigsState *fsd_enter(void) {
    EigsState *back = cur_state();
    g_fsd_busy = 1;
    eigs_thread_switch(g_fs_state);
    return back;
}

static void fsd_leave(EigsState *back) {
    eigs_thread_switch(back);
    g_fsd_busy = 0;
}

/* run one expression on the fsd; returns its numeric result (or `fail`
 * on error / non-number). Caller must already be SWITCHED to the fsd. */
static double fsd_eval_num(const char *src, double fail) {
    EigsValue *v = eigs_eval_string(src);
    double out = fail;
    if (eigs_has_error()) eigs_clear_error();
    else if (v && eigs_value_type(v) == EIGS_TYPE_NUM) out = eigs_value_as_num(v);
    if (v) eigs_value_release(v);
    return out;
}

static void fsd_set_str(const char *g, const char *s) {
    EigsValue *v = eigs_value_new_string(s);
    eigs_set_global(g, v);
    eigs_value_release(v);
}

static int fs_ensure_mounted(void) {
    if (!g_fs_state) return 0;
    if (g_fs_mounted) return 1;
    EigsState *back = fsd_enter();
    if (fsd_eval_num("regionfs.mount of []", 0) == 1.0) g_fs_mounted = 1;
    fsd_leave(back);
    return g_fs_mounted;
}

/* copy a path argument (string value) into a C buffer; 1 on success */
static int path_arg(EigsValue *v, char *dst, int cap) {
    const char *s = eigs_value_as_string(v);
    if (!s || !s[0]) return 0;
    int i = 0;
    for (; s[i] && i < cap - 1; i++) dst[i] = s[i];
    dst[i] = 0;
    return 1;
}

/* extract list|buffer byte content into a fresh C buffer (caller frees) */
static unsigned char *bytes_arg(EigsValue *d, int *out_n) {
    int n, i;
    unsigned char *tmp;
    if (!d) return NULL;
    if (eigs_value_type(d) == EIGS_TYPE_BUFFER) {
        n = eigs_value_buffer_len(d);
        tmp = malloc(n > 0 ? (size_t)n : 1);
        if (!tmp) return NULL;
        for (i = 0; i < n; i++) tmp[i] = (unsigned char)eigs_value_buffer_get(d, i);
    } else if (eigs_value_type(d) == EIGS_TYPE_LIST) {
        n = eigs_value_list_len(d);
        tmp = malloc(n > 0 ? (size_t)n : 1);
        if (!tmp) return NULL;
        for (i = 0; i < n; i++) {
            EigsValue *it = eigs_value_list_get(d, i);
            tmp[i] = (unsigned char)((it && eigs_value_type(it) == EIGS_TYPE_NUM)
                                         ? eigs_value_as_num(it) : 0.0);
            if (it) eigs_value_release(it);
        }
    } else return NULL;
    *out_n = n;
    return tmp;
}

/* read a whole region into a C staging buffer via the fsd; returns the
 * buffer (caller frees) and its length, or NULL when absent/unmounted. */
static unsigned char *fs_read_staged(const char *path, int *out_n) {
    if (!fs_ensure_mounted()) return NULL;
    EigsState *back = fsd_enter();
    fsd_set_str("fsd_path", path);
    EigsValue *v = eigs_eval_string("regionfs.read_bytes_buf of fsd_path");
    unsigned char *tmp = NULL;
    int n = 0;
    if (eigs_has_error()) eigs_clear_error();
    else if (v && eigs_value_type(v) == EIGS_TYPE_BUFFER) {
        n = eigs_value_buffer_len(v);
        tmp = malloc(n > 0 ? (size_t)n + 1 : 1);
        if (tmp)
            for (int i = 0; i < n; i++)
                tmp[i] = (unsigned char)eigs_value_buffer_get(v, i);
    }
    if (v) eigs_value_release(v);               /* released on the fsd */
    fsd_leave(back);
    if (tmp) *out_n = n;
    return tmp;
}

/* stage C bytes into the fsd and run a write; returns the op's result */
static int fs_write_staged(const char *path, const unsigned char *bytes,
                           int n, int append) {
    if (!fs_ensure_mounted()) return 0;
    EigsState *back = fsd_enter();
    fsd_set_str("fsd_path", path);
    EigsValue *bv = eigs_value_new_buffer(n);
    for (int i = 0; i < n; i++) eigs_value_buffer_set(bv, i, (double)bytes[i]);
    eigs_set_global("fsd_data", bv);
    eigs_value_release(bv);
    EigsValue *ap = eigs_value_new_num(append ? 1 : 0);
    eigs_set_global("fsd_app", ap);
    eigs_value_release(ap);
    int ok = fsd_eval_num(
        "regionfs.write_bytes of [fsd_path, fsd_data, fsd_app]", 0) == 1.0;
    EigsValue *nul = eigs_value_new_null();     /* drop the staged bytes */
    eigs_set_global("fsd_data", nul);
    eigs_value_release(nul);
    fsd_leave(back);
    return ok;
}

static EigsValue *b_fs_file_exists(EigsValue *a) {
    char p[64];
    if (!path_arg(a, p, sizeof p) || !fs_ensure_mounted())
        return eigs_value_new_num(0);
    EigsState *back = fsd_enter();
    fsd_set_str("fsd_path", p);
    double r = fsd_eval_num("regionfs.file_exists of fsd_path", 0);
    fsd_leave(back);
    return eigs_value_new_num(r);
}

static EigsValue *b_fs_read_bytes_buf(EigsValue *a) {
    char p[64];
    int n = 0;
    if (!path_arg(a, p, sizeof p)) return eigs_value_new_null();
    unsigned char *tmp = fs_read_staged(p, &n);
    if (!tmp) return eigs_value_new_null();
    EigsValue *out = eigs_value_new_buffer(n);
    for (int i = 0; i < n; i++) eigs_value_buffer_set(out, i, (double)tmp[i]);
    free(tmp);
    return out;
}

static EigsValue *b_fs_read_text(EigsValue *a) {
    char p[64];
    int n = 0;
    if (!path_arg(a, p, sizeof p)) return eigs_value_new_null();
    unsigned char *tmp = fs_read_staged(p, &n);
    if (!tmp) return eigs_value_new_null();
    tmp[n] = 0;                    /* C-string reality: truncates at NUL */
    EigsValue *out = eigs_value_new_string((const char *)tmp);
    free(tmp);
    return out;
}

static EigsValue *b_fs_write_bytes(EigsValue *a) {   /* [path, bytes {,app}] */
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    char p[64];
    EigsValue *pv = eigs_value_list_get(a, 0);
    int okp = pv && path_arg(pv, p, sizeof p);
    if (pv) eigs_value_release(pv);
    if (!okp) return eigs_value_new_num(0);
    EigsValue *dv = eigs_value_list_get(a, 1);
    int n = 0;
    unsigned char *tmp = bytes_arg(dv, &n);
    if (dv) eigs_value_release(dv);
    if (!tmp) return eigs_value_new_num(0);
    int append = eigs_value_list_len(a) >= 3 && list_num(a, 2) != 0.0;
    int ok = fs_write_staged(p, tmp, n, append);
    free(tmp);
    return eigs_value_new_num(ok);
}

static EigsValue *b_fs_write_text(EigsValue *a) {    /* [path, str] */
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    char p[64];
    EigsValue *pv = eigs_value_list_get(a, 0);
    int okp = pv && path_arg(pv, p, sizeof p);
    if (pv) eigs_value_release(pv);
    if (!okp) return eigs_value_new_num(0);
    EigsValue *sv = eigs_value_list_get(a, 1);
    const char *s = sv ? eigs_value_as_string(sv) : NULL;
    if (!s) {
        if (sv) eigs_value_release(sv);
        return eigs_value_new_num(0);
    }
    int n = (int)strlen(s);
    unsigned char *tmp = malloc(n > 0 ? (size_t)n : 1);
    if (!tmp) {
        eigs_value_release(sv);
        return eigs_value_new_num(0);
    }
    for (int i = 0; i < n; i++) tmp[i] = (unsigned char)s[i];
    eigs_value_release(sv);
    int ok = fs_write_staged(p, tmp, n, 0);
    free(tmp);
    return eigs_value_new_num(ok);
}

static EigsValue *b_fs_rename(EigsValue *a) {        /* [old, new] */
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    char po[64], pn[64];
    EigsValue *ov = eigs_value_list_get(a, 0);
    EigsValue *nv = eigs_value_list_get(a, 1);
    int ok = ov && nv && path_arg(ov, po, sizeof po) && path_arg(nv, pn, sizeof pn);
    if (ov) eigs_value_release(ov);
    if (nv) eigs_value_release(nv);
    if (!ok || !fs_ensure_mounted()) return eigs_value_new_num(0);
    EigsState *back = fsd_enter();
    fsd_set_str("fsd_path", po);
    fsd_set_str("fsd_path2", pn);
    double r = fsd_eval_num("regionfs.rename of [fsd_path, fsd_path2]", 0);
    fsd_leave(back);
    return eigs_value_new_num(r);
}

static EigsValue *b_fs_remove_file(EigsValue *a) {
    char p[64];
    if (!path_arg(a, p, sizeof p) || !fs_ensure_mounted())
        return eigs_value_new_num(0);
    EigsState *back = fsd_enter();
    fsd_set_str("fsd_path", p);
    double r = fsd_eval_num("regionfs.remove_file of fsd_path", 0);
    fsd_leave(back);
    return eigs_value_new_num(r);
}

static EigsValue *b_fs_mount(EigsValue *a) {         /* explicit (re)mount */
    (void)a;
    g_fs_mounted = 0;
    return eigs_value_new_num(fs_ensure_mounted());
}

static EigsValue *b_fs_format(EigsValue *a) {        /* the manual act */
    (void)a;
    if (!g_fs_state) return eigs_value_new_num(0);
    EigsState *back = fsd_enter();
    double r = fsd_eval_num("regionfs.format of []", 0);
    fsd_leave(back);
    if (r == 1.0) g_fs_mounted = 1;
    return eigs_value_new_num(r);
}

static void register_fs(void) {
    eigs_register_function("file_exists",    b_fs_file_exists);
    eigs_register_function("read_text",      b_fs_read_text);
    eigs_register_function("read_bytes_buf", b_fs_read_bytes_buf);
    eigs_register_function("write_text",     b_fs_write_text);
    eigs_register_function("write_bytes",    b_fs_write_bytes);
    eigs_register_function("rename",         b_fs_rename);
    eigs_register_function("remove_file",    b_fs_remove_file);
    eigs_register_function("fs_mount",       b_fs_mount);
    eigs_register_function("fs_format",      b_fs_format);
}

/* ---- M11: the machine journal ----
 * The trace tape (upstream seam, PR #365) recorded to the M10 store.
 * journal_on installs a byte sink that only APPENDS to a kheap buffer
 * (the sink fires mid-eval — never re-enter the runtime from it);
 * journal_off uninstalls the sink and writes the buffer to the store
 * file "journal" through the fsd. journal_replay loads that file and
 * installs it as the replay tape: every instrumented nondet builtin
 * (ticks, heap vitals, mouse, gfx_ticks) then returns the RECORDED
 * machine readings in order — a journaled session re-derives the same
 * values on a different day's machine. journal_stop clears replay.
 * Contract (tidelog's): deterministic given the durable input — replay
 * assumes the same store state the recording ran against. */
static char  *g_jrn_buf;
static size_t g_jrn_len, g_jrn_cap;
static int    g_jrn_on;          /* MANUAL recording (journal_on..off) */

/* M11 phase 3: the auto-journal ring — the machine as its own flight
 * recorder. When a store mounted at boot, the sink is always on and
 * the tape accumulates in THIS RAM ring: at AUTO_CAP the buffer slides
 * to its newest half, cutting at an I-record boundary so the surviving
 * tail is replay-coherent (a tail with no I records drives nothing —
 * orphan N records are never consumed; harmless). Between evals the
 * REPL loop truncate-writes the whole ring to the store file
 * "autolog" — the file IS a snapshot of the ring, so there is no
 * rotation machinery and a crash loses only the since-last-eval tail.
 * After an unexpected death: journal_replay of "autolog". */
#define JRN_AUTO_CAP 65536
static int g_auto_on;            /* auto ring active (store mounted) */
static int g_jrn_dirty;          /* ring has bytes the store hasn't seen */
static int g_rp_driving;         /* a session replay is being driven */

/* M11 phase 2: pending session-replay lines, parsed from the journal's
 * I records by journal_replay and DRIVEN BY THE REPL LOOP between
 * evals (never from inside a builtin — that would nest the VM on the
 * primary state). */
static char **g_rp_lines;
static int    g_rp_count;

static void jrn_sink(const char *bytes, size_t len, void *ud) {
    (void)ud;
    if (g_fsd_busy) return;      /* fs-daemon machinery is not session activity */
    if (g_jrn_len + len > g_jrn_cap) {
        size_t nc = g_jrn_cap ? g_jrn_cap * 2 : 4096;
        while (nc < g_jrn_len + len) nc *= 2;
        char *nb = realloc(g_jrn_buf, nc);
        if (!nb) return;                    /* full: drop (never crash) */
        g_jrn_buf = nb;
        g_jrn_cap = nc;
    }
    memcpy(g_jrn_buf + g_jrn_len, bytes, len);
    g_jrn_len += len;
    g_jrn_dirty = 1;
    /* ring bound: slide to the newest half, cutting where an I record
     * starts so the tail replays coherently */
    if (g_jrn_len > JRN_AUTO_CAP) {
        size_t cut = 0;
        for (size_t i = g_jrn_len / 2; i + 2 < g_jrn_len; i++)
            if (g_jrn_buf[i] == '\n' && g_jrn_buf[i + 1] == 'I' &&
                g_jrn_buf[i + 2] == ' ') { cut = i + 1; break; }
        if (!cut)                          /* no I record in the tail: any
                                              line boundary is safe */
            for (size_t i = g_jrn_len / 2; i < g_jrn_len; i++)
                if (g_jrn_buf[i] == '\n') { cut = i + 1; break; }
        if (cut && cut < g_jrn_len) {
            memmove(g_jrn_buf, g_jrn_buf + cut, g_jrn_len - cut);
            g_jrn_len -= cut;
        }
    }
}

/* Flush the ring to the store — called by the REPL loop BETWEEN evals
 * (the fs delegate switches states; never do this mid-eval). Truncate
 * mode: the file is a snapshot of the ring. */
static void jrn_auto_flush(void) {
    if (!g_auto_on || g_jrn_on || !g_jrn_dirty || !g_jrn_len) return;
    if (fs_write_staged("autolog", (unsigned char *)g_jrn_buf,
                        (int)g_jrn_len, 0))
        g_jrn_dirty = 0;
}

/* Record one submitted REPL source unit as an `I <escaped>` record in
 * the journal, INTERLEAVED with the runtime's own L/A/N records (the
 * runtime's replay reader skips anything that isn't `N ...`, so the
 * tape stays valid for it). Newlines and backslashes are escaped so a
 * multi-line block is one record line. journal_* meta-lines are NOT
 * recorded — a replayed session must not rewrite the journal it is
 * replaying from. */
static void jrn_record_input(const char *src) {
    if ((!g_jrn_on && !g_auto_on) || g_rp_driving) return;
    if (!src || !src[0]) return;
    if (strncmp(src, "journal_", 8) == 0) return;
    jrn_sink("I ", 2, 0);
    for (const char *p = src; *p; p++) {
        if (*p == '\n')      jrn_sink("\\n", 2, 0);
        else if (*p == '\\') jrn_sink("\\\\", 2, 0);
        else                 jrn_sink(p, 1, 0);
    }
    jrn_sink("\n", 1, 0);
}

static EigsValue *b_journal_on(EigsValue *a) {
    /* the MANUAL bracket: a deliberate session saved to "journal".
     * The auto ring pauses (its content is discarded — the manual
     * session records clean) and resumes at journal_off. */
    (void)a;
    g_jrn_len = 0;
    g_jrn_dirty = 0;
    g_jrn_on = 1;
    eigs_set_trace_sink(jrn_sink, 0);
    serial_write("[m11] journal: recording (inputs + machine readings)\n");
    return eigs_value_new_num(1);
}

static EigsValue *b_journal_off(EigsValue *a) {
    (void)a;
    if (!g_auto_on) eigs_set_trace_sink(0, 0);  /* flushes pending line */
    g_jrn_on = 0;
    double n = (double)g_jrn_len;
    int ok = g_jrn_len > 0 &&
             fs_write_staged("journal", (unsigned char *)g_jrn_buf,
                             (int)g_jrn_len, 0);
    g_jrn_len = 0;                          /* auto ring restarts clean */
    g_jrn_dirty = 0;
    serial_write(ok ? "[m11] journal: written to the store\n"
                    : "[m11] journal: NOT written (empty or store refused)\n");
    return eigs_value_new_num(ok ? n : 0);
}

/* unescape one I record body [s, s+n) into a fresh C string */
static char *jrn_unescape(const char *s, int n) {
    char *out = malloc((size_t)n + 1);
    if (!out) return 0;
    int o = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n) {
            i++;
            out[o++] = (s[i] == 'n') ? '\n' : s[i];
        } else out[o++] = s[i];
    }
    out[o] = 0;
    return out;
}

static void rp_clear(void) {
    for (int i = 0; i < g_rp_count; i++) free(g_rp_lines[i]);
    free(g_rp_lines);
    g_rp_lines = 0;
    g_rp_count = 0;
}

static EigsValue *b_journal_replay(EigsValue *a) {
    /* optional string arg: which journal file — "journal" (the manual
     * save, default) or "autolog" (the ring: what a crashed session
     * was doing). */
    char path[64] = "journal";
    if (a && eigs_value_type(a) == EIGS_TYPE_STR) {
        if (!path_arg(a, path, sizeof path)) return eigs_value_new_num(0);
    }
    if (g_jrn_on) {
        serial_write("[m11] journal_replay REFUSED while recording\n");
        return eigs_value_new_num(0);
    }
    int n = 0;
    unsigned char *tape = fs_read_staged(path, &n);
    if (!tape) return eigs_value_new_num(0);
    int ok = eigs_set_replay_tape((const char *)tape, (size_t)n, 0);
    /* parse the I records: the session's input lines, in order */
    rp_clear();
    if (ok) {
        int cap = 0;
        int i = 0;
        while (i < n) {
            int e = i;
            while (e < n && tape[e] != '\n') e++;
            if (e - i > 2 && tape[i] == 'I' && tape[i + 1] == ' ') {
                char *src = jrn_unescape((const char *)tape + i + 2, e - i - 2);
                if (src) {
                    if (g_rp_count == cap) {
                        cap = cap ? cap * 2 : 8;
                        char **nl = realloc(g_rp_lines,
                                            (size_t)cap * sizeof(char *));
                        if (!nl) { free(src); break; }
                        g_rp_lines = nl;
                    }
                    g_rp_lines[g_rp_count++] = src;
                }
            }
            i = e + 1;
        }
    }
    free(tape);                             /* the runtime copied it */
    serial_write(ok ? "[m11] journal: session loaded — replay begins after this eval\n"
                    : "[m11] journal: replay tape rejected\n");
    return eigs_value_new_num(ok ? (double)g_rp_count : 0);
}

static EigsValue *b_journal_stop(EigsValue *a) {
    (void)a;
    eigs_set_replay_tape(0, 0, 0);
    rp_clear();
    return eigs_value_new_num(1);
}

static void register_journal(void) {
    eigs_register_function("journal_on",     b_journal_on);
    eigs_register_function("journal_off",    b_journal_off);
    eigs_register_function("journal_replay", b_journal_replay);
    eigs_register_function("journal_stop",   b_journal_stop);
}

/* M9.5: pixels and a pointer — ext_gfx's second backend. The same
 * builtin names Tidepool's loop calls hosted (gfx_open/clear/rect/line/
 * point/circle/text/present/poll/ticks/delay), backed by the LFB shadow
 * buffer (fb.c) instead of SDL2, and gfx_poll fed by the PS/2 drivers
 * with hosted-shaped event dicts ({"type":"mousemove","x":..,"y":..},
 * {"type":"keydown","key":"q"}, SDL button numbering 1/2/3).
 *
 * One display, no windows (until MD): gfx_open suspends the console —
 * the grid still records, serial still logs — and gfx_close repaints
 * it. Subset notes: window size/title are fixed by the boot mode (the
 * [w,h,title] args are accepted, ignored). Since the MD groundwork the
 * event ring carries keydown+keyup with modifiers AND wheel. */
static double list_num(EigsValue *l, int i) {
    EigsValue *v = eigs_value_list_get(l, i);
    double d = (v && eigs_value_type(v) == EIGS_TYPE_NUM)
                   ? eigs_value_as_num(v) : 0.0;
    if (v) eigs_value_release(v);
    return d;
}

static void list_str(EigsValue *l, int i, char *buf, int cap) {
    buf[0] = 0;
    EigsValue *v = eigs_value_list_get(l, i);
    if (v && eigs_value_type(v) == EIGS_TYPE_STR) {
        const char *s = eigs_value_as_string(v);
        int k = 0;
        for (; s && s[k] && k < cap - 1; k++) buf[k] = s[k];
        buf[k] = 0;
    }
    if (v) eigs_value_release(v);
}

static int arg_list_min(EigsValue *a, int n) {
    return a && eigs_value_type(a) == EIGS_TYPE_LIST &&
           eigs_value_list_len(a) >= n;
}

static int g_gfx_open;

/* WM clip (hosted-parity gfx_clip): lets the primary state draw
 * window CONTENT safely bounded — e.g. the REPL window's transcript,
 * which the WM renders on the repl task's behalf. */
static int g_wm_clip_on, g_wm_clip[4];

/* The drawing context (MD): who is drawing decides where the marks
 * land. Primary = the whole screen (or its gfx_clip rectangle if one
 * is set). A task mid-step = translated into and clipped to its
 * viewport — a window's content cannot escape the window. A task with
 * NO viewport draws nowhere (zero-area clip). Returns the coordinate
 * offset to apply. */
static void gfx_ctx(int *ox, int *oy) {
    *ox = 0; *oy = 0;
    if (g_active_task < 0) {
        if (g_wm_clip_on)
            fb_gfx_clip(g_wm_clip[0], g_wm_clip[1], g_wm_clip[2], g_wm_clip[3]);
        else
            fb_gfx_clip_reset();
        return;
    }
    if (g_tasks[g_active_task].has_view) {
        *ox = g_tasks[g_active_task].vx;
        *oy = g_tasks[g_active_task].vy;
        fb_gfx_clip(g_tasks[g_active_task].vx, g_tasks[g_active_task].vy,
                    g_tasks[g_active_task].vw, g_tasks[g_active_task].vh);
    } else {
        fb_gfx_clip(0, 0, 0, 0);
    }
}

static EigsValue *b_gfx_open(EigsValue *a) {
    (void)a;                          /* [w,h,title] accepted; mode is fixed */
    if (!fb_active()) return eigs_value_new_num(0);   /* hosted-parity fail */
    if (g_active_task >= 0)           /* a window is already "open": its
                                         viewport (1 if the WM gave it one) */
        return eigs_value_new_num(g_tasks[g_active_task].has_view ? 1 : 0);
    vga_suspend(1);
    kbd_gfx_mode(1);                  /* keys become events, not REPL chars */
    fb_gfx_clear(0, 0, 0);
    fb_present();
    g_gfx_open = 1;
    return eigs_value_new_num(1);
}

static EigsValue *b_gfx_close(EigsValue *a) {
    (void)a;
    if (g_active_task >= 0) return eigs_value_new_null();   /* WM's call, not a window's */
    g_gfx_open = 0;
    kbd_gfx_mode(0);
    vga_suspend(0);                   /* repaints the console grid */
    return eigs_value_new_null();
}

static EigsValue *b_gfx_clear(EigsValue *a) {
    int r = 0, g = 0, b = 0;
    if (arg_list_min(a, 3)) {
        r = (int)list_num(a, 0); g = (int)list_num(a, 1); b = (int)list_num(a, 2);
    }
    if (g_active_task >= 0) {         /* a window clears ITS rectangle */
        int ox, oy;
        gfx_ctx(&ox, &oy);
        if (g_tasks[g_active_task].has_view)
            fb_gfx_fill(ox, oy, g_tasks[g_active_task].vw,
                        g_tasks[g_active_task].vh, r, g, b, 255);
    } else {
        fb_gfx_clip_reset();
        fb_gfx_clear(r, g, b);
    }
    return eigs_value_new_null();
}

static EigsValue *b_gfx_rect(EigsValue *a) {
    if (!arg_list_min(a, 7)) return eigs_value_new_null();
    int ox, oy;
    gfx_ctx(&ox, &oy);
    int alpha = eigs_value_list_len(a) >= 8 ? (int)list_num(a, 7) : 255;
    fb_gfx_fill(ox + (int)list_num(a, 0), oy + (int)list_num(a, 1),
                (int)list_num(a, 2), (int)list_num(a, 3),
                (int)list_num(a, 4), (int)list_num(a, 5),
                (int)list_num(a, 6), alpha);
    return eigs_value_new_null();
}

static EigsValue *b_gfx_line(EigsValue *a) {
    if (!arg_list_min(a, 7)) return eigs_value_new_null();
    int ox, oy;
    gfx_ctx(&ox, &oy);
    fb_gfx_line(ox + (int)list_num(a, 0), oy + (int)list_num(a, 1),
                ox + (int)list_num(a, 2), oy + (int)list_num(a, 3),
                (int)list_num(a, 4), (int)list_num(a, 5),
                (int)list_num(a, 6));
    return eigs_value_new_null();
}

static EigsValue *b_gfx_point(EigsValue *a) {
    if (!arg_list_min(a, 5)) return eigs_value_new_null();
    int ox, oy;
    gfx_ctx(&ox, &oy);
    fb_gfx_point(ox + (int)list_num(a, 0), oy + (int)list_num(a, 1),
                 (int)list_num(a, 2), (int)list_num(a, 3),
                 (int)list_num(a, 4));
    return eigs_value_new_null();
}

static EigsValue *b_gfx_circle(EigsValue *a) {
    if (!arg_list_min(a, 6)) return eigs_value_new_null();
    int ox, oy;
    gfx_ctx(&ox, &oy);
    fb_gfx_circle(ox + (int)list_num(a, 0), oy + (int)list_num(a, 1),
                  (int)list_num(a, 2), (int)list_num(a, 3),
                  (int)list_num(a, 4), (int)list_num(a, 5));
    return eigs_value_new_null();
}

static EigsValue *b_gfx_text(EigsValue *a) {
    if (!arg_list_min(a, 6)) return eigs_value_new_null();
    int ox, oy;
    gfx_ctx(&ox, &oy);
    char text[256];
    list_str(a, 2, text, sizeof text);
    int scale = eigs_value_list_len(a) >= 7 ? (int)list_num(a, 6) : 1;
    fb_gfx_text(ox + (int)list_num(a, 0), oy + (int)list_num(a, 1), text,
                (int)list_num(a, 3), (int)list_num(a, 4),
                (int)list_num(a, 5), scale);
    return eigs_value_new_null();
}

static EigsValue *b_gfx_present(EigsValue *a) {
    (void)a;
    fb_present();
    return eigs_value_new_null();
}

static EigsValue *b_gfx_ticks(EigsValue *a) {
    (void)a;
    uint32_t hz = timer_hz(); if (!hz) hz = 100;
    return vital_nondet("gfx_ticks",
                        (double)(timer_ticks() * 1000ULL / hz));
}

static EigsValue *b_gfx_delay(EigsValue *a) {
    vitals_sample_maybe();               /* M11: gfx sessions sample too */
    if (a && eigs_value_type(a) == EIGS_TYPE_NUM)
        usleep((unsigned)(eigs_value_as_num(a) * 1000.0));
    return eigs_value_new_null();
}

static EigsValue *b_gfx_title(EigsValue *a) {
    (void)a;                                     /* no window to retitle */
    return eigs_value_new_null();
}

/* gfx_clip of [x,y,w,h] / gfx_clip of null — hosted ext_gfx parity.
 * Meaningful from the primary state (the WM bounding its own drawing);
 * a task is always viewport-clipped and cannot widen that. */
static EigsValue *b_gfx_clip(EigsValue *a) {
    if (g_active_task >= 0) return eigs_value_new_null();
    if (arg_list_min(a, 4)) {
        g_wm_clip[0] = (int)list_num(a, 0);
        g_wm_clip[1] = (int)list_num(a, 1);
        g_wm_clip[2] = (int)list_num(a, 2);
        g_wm_clip[3] = (int)list_num(a, 3);
        g_wm_clip_on = 1;
    } else {
        g_wm_clip_on = 0;
    }
    return eigs_value_new_null();
}

/* Event pump: PS/2 packets become SDL-shaped events. A packet can move
 * AND flip buttons AND scroll, so one packet may fan out to several
 * events — a small queue decouples them for one-at-a-time gfx_poll. */
#define GEVQ 32
static struct { uint8_t type; int x, y, btn; } g_evq[GEVQ];
static unsigned g_evh, g_evt;
static int g_btn_prev;
enum { GE_MOVE = 1, GE_DOWN, GE_UP, GE_WHEEL };

static void ev_push(int type, int x, int y, int btn) {
    unsigned next = (g_evh + 1) % GEVQ;
    if (next == g_evt) return;
    g_evq[g_evh].type = (uint8_t)type;
    g_evq[g_evh].x = x; g_evq[g_evh].y = y; g_evq[g_evh].btn = btn;
    g_evh = next;
}

static const int sdl_btn[3] = { 1, 3, 2 };       /* PS/2 L,R,M -> SDL 1,3,2 */

static void pump_mouse(void) {
    int x, y, btn, moved, wz;
    while (((g_evh + 5) % GEVQ) != g_evt &&      /* room for a full fan-out */
           mouse_next_packet(&x, &y, &btn, &moved, &wz)) {
        if (moved) ev_push(GE_MOVE, x, y, 0);
        if (wz)                                  /* PS/2 z+ = toward the user
                                                    = scroll down; SDL y+ =
                                                    scroll up (Linux psmouse
                                                    negates the same way) */
            ev_push(GE_WHEEL, 0, -wz, 0);
        int changed = btn ^ g_btn_prev;
        for (int bit = 0; bit < 3; bit++)
            if (changed & (1 << bit))
                ev_push((btn & (1 << bit)) ? GE_DOWN : GE_UP, x, y, sdl_btn[bit]);
        g_btn_prev = btn;
    }
}

static void dset_num(EigsValue *d, const char *k, double n) {
    EigsValue *v = eigs_value_new_num(n);
    eigs_value_dict_set(d, k, v);
    eigs_value_release(v);
}
static void dset_str(EigsValue *d, const char *k, const char *s) {
    EigsValue *v = eigs_value_new_string(s);
    eigs_value_dict_set(d, k, v);
    eigs_value_release(v);
}

static const char *key_name(int c, char *one) {
    switch (c) {
    case '\n':       return "return";
    case 27:         return "escape";
    case ' ':        return "space";
    case '\b':       return "backspace";
    case '\t':       return "tab";
    case KEY_UP:     return "up";
    case KEY_DOWN:   return "down";
    case KEY_LEFT:   return "left";
    case KEY_RIGHT:  return "right";
    case KEY_HOME:   return "home";
    case KEY_END:    return "end";
    case KEY_DELETE: return "delete";
    case KEY_PGUP:   return "pageup";
    case KEY_PGDN:   return "pagedown";
    default:   one[0] = (char)c; one[1] = 0; return one;
    }
}

static EigsValue *b_gfx_poll(EigsValue *a) {
    (void)a;
    if (!g_gfx_open) return eigs_value_new_null();
    if (g_active_task >= 0) return eigs_value_new_null();  /* the WM owns
                                       input; windows get task_key codes */
    pump_mouse();
    if (g_evt != g_evh) {
        int type = g_evq[g_evt].type, x = g_evq[g_evt].x,
            y = g_evq[g_evt].y, btn = g_evq[g_evt].btn;
        g_evt = (g_evt + 1) % GEVQ;
        EigsValue *d = eigs_value_new_dict(4);
        switch (type) {
        case GE_MOVE:
            dset_str(d, "type", "mousemove");
            dset_num(d, "x", (double)x);
            dset_num(d, "y", (double)y);
            break;
        case GE_WHEEL:                    /* hosted shape: x/y = scroll amounts */
            dset_str(d, "type", "wheel");
            dset_num(d, "x", (double)x);
            dset_num(d, "y", (double)y);
            break;
        default:
            dset_str(d, "type", type == GE_DOWN ? "mousedown" : "mouseup");
            dset_num(d, "button", (double)btn);
            dset_num(d, "x", (double)x);
            dset_num(d, "y", (double)y);
            break;
        }
        return d;
    }
    int code, down, mods;
    if (kbd_next_event(&code, &down, &mods)) {
        char one[2];
        EigsValue *d = eigs_value_new_dict(4);
        dset_str(d, "type", down ? "keydown" : "keyup");
        dset_str(d, "key", key_name(code, one));
        dset_num(d, "code", (double)code);   /* forwardable via task_key */
        dset_num(d, "shift", (mods & KMOD_SHIFT) ? 1 : 0);
        dset_num(d, "ctrl",  (mods & KMOD_CTRL)  ? 1 : 0);
        dset_num(d, "alt",   (mods & KMOD_ALT)   ? 1 : 0);
        return d;
    }
    return eigs_value_new_null();
}

/* ---- MD window seams ----
 * task_view [id,x,y,w,h] — the WM assigns a viewport (window content
 * area); the task's gfx calls land there and nowhere else.
 * task_key [id, code] — the WM forwards a key to a window; win_key
 * (task-side) pops it. Numbers only, per the cross-state rule.
 * task_wheel [id, dy] — the WM forwards scroll to a window; deltas
 * ACCUMULATE (scroll is additive) and win_wheel (task-side) reads and
 * clears the sum (0 = nothing since the last read).
 * win_w/win_h (task-side) — the window's content size. */
static EigsValue *b_task_view(EigsValue *a) {
    if (!arg_list_min(a, 5)) return eigs_value_new_num(0);
    EigsValue *idv = eigs_value_list_get(a, 0);
    int id = task_id_of(idv);
    if (idv) eigs_value_release(idv);
    if (id < 0) return eigs_value_new_num(0);
    g_tasks[id].vx = (int)list_num(a, 1);
    g_tasks[id].vy = (int)list_num(a, 2);
    g_tasks[id].vw = (int)list_num(a, 3);
    g_tasks[id].vh = (int)list_num(a, 4);
    g_tasks[id].has_view = (g_tasks[id].vw > 0 && g_tasks[id].vh > 0);
    return eigs_value_new_num(g_tasks[id].has_view ? 1 : 0);
}

static EigsValue *b_task_key(EigsValue *a) {
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    EigsValue *idv = eigs_value_list_get(a, 0);
    int id = task_id_of(idv);
    if (idv) eigs_value_release(idv);
    if (id < 0) return eigs_value_new_num(0);
    unsigned next = (g_tasks[id].kq_head + 1) % TASK_KEYQ;
    if (next == g_tasks[id].kq_tail) return eigs_value_new_num(0);  /* full */
    g_tasks[id].keyq[g_tasks[id].kq_head] = (int)list_num(a, 1);
    g_tasks[id].kq_head = next;
    return eigs_value_new_num(1);
}

static EigsValue *b_task_wheel(EigsValue *a) {  /* [id, dy] — accumulate */
    if (!arg_list_min(a, 2)) return eigs_value_new_num(0);
    EigsValue *idv = eigs_value_list_get(a, 0);
    int id = task_id_of(idv);
    if (idv) eigs_value_release(idv);
    if (id < 0) return eigs_value_new_num(0);
    int w = g_tasks[id].wheel + (int)list_num(a, 1);
    if (w >  32767) w =  32767;   /* a window that never reads can't overflow */
    if (w < -32767) w = -32767;
    g_tasks[id].wheel = w;
    return eigs_value_new_num(1);
}

static EigsValue *b_win_wheel(EigsValue *a) {   /* read-and-clear the delta */
    (void)a;
    if (g_active_task < 0) return eigs_value_new_num(0);
    int w = g_tasks[g_active_task].wheel;
    g_tasks[g_active_task].wheel = 0;
    return eigs_value_new_num((double)w);
}

static EigsValue *b_win_key(EigsValue *a) {
    (void)a;
    if (g_active_task < 0) return eigs_value_new_num(-1);
    unsigned t = g_tasks[g_active_task].kq_tail;
    if (t == g_tasks[g_active_task].kq_head) return eigs_value_new_num(-1);
    int code = g_tasks[g_active_task].keyq[t];
    g_tasks[g_active_task].kq_tail = (t + 1) % TASK_KEYQ;
    return eigs_value_new_num((double)code);
}

static EigsValue *b_win_w(EigsValue *a) {
    (void)a;
    int id = g_active_task;
    return eigs_value_new_num(id >= 0 && g_tasks[id].has_view
                                  ? (double)g_tasks[id].vw : 0);
}

static EigsValue *b_win_h(EigsValue *a) {
    (void)a;
    int id = g_active_task;
    return eigs_value_new_num(id >= 0 && g_tasks[id].has_view
                                  ? (double)g_tasks[id].vh : 0);
}

/* task_eval [id, "src"] — evaluate a line on a WINDOW'S OWN state and
 * return the result formatted as a string (copied across the state
 * boundary byte-by-byte — the cross-state rule extends from numbers
 * to by-value strings, never pointers). This is the REPL-as-a-window
 * seam: the WM owns the input line, the window owns the bindings.
 * The task's state is idle between steps, so this is the same
 * outside-in eval task_spawn/task_step already do — no VM nesting.
 * g_active_task is set during the eval: a typed `gfx_circle of [...]`
 * draws INTO that window, clipped, like everything else it runs. */
static EigsValue *b_task_eval(EigsValue *a) {
    if (!arg_list_min(a, 2)) return eigs_value_new_string("");
    EigsValue *idv = eigs_value_list_get(a, 0);
    int id = task_id_of(idv);
    if (idv) eigs_value_release(idv);
    char src[512];
    list_str(a, 1, src, sizeof src);
    if (id < 0 || !src[0]) return eigs_value_new_string("");

    char out[256];
    out[0] = 0;
    eigs_thread_switch(g_tasks[id].st);
    g_active_task = id;
    EigsValue *v = eigs_eval_string(src);
    if (eigs_has_error()) {
        snprintf(out, sizeof out, "error: %s", eigs_last_error_message());
        eigs_clear_error();
    } else if (v) {
        switch (eigs_value_type(v)) {
        case EIGS_TYPE_NUM:
            snprintf(out, sizeof out, "%g", eigs_value_as_num(v));
            break;
        case EIGS_TYPE_STR:
            snprintf(out, sizeof out, "%s", eigs_value_as_string(v));
            break;
        case EIGS_TYPE_NULL:
            break;
        default:
            snprintf(out, sizeof out, "<value>");
            break;
        }
    }
    if (v) eigs_value_release(v);               /* released on the TASK state */
    g_active_task = -1;
    eigs_thread_switch(g_primary);
    if (!stack_canary_ok()) {
        con_write("PANIC: boot-stack overflow (canary trampled by a "
                  "window eval). Halting.\n");
        speaker_beeps(3);
        for (;;) __asm__ volatile ("cli; hlt");
    }
    return eigs_value_new_string(out);
}

static EigsValue *b_mouse_x(EigsValue *a)  { (void)a; return vital_nondet("mouse_x", (double)mouse_x()); }
static EigsValue *b_mouse_y(EigsValue *a)  { (void)a; return vital_nondet("mouse_y", (double)mouse_y()); }
static EigsValue *b_mouse_btn(EigsValue *a){ (void)a; return vital_nondet("mouse_buttons", (double)mouse_buttons()); }

static void register_gfx(void) {
    eigs_register_function("gfx_open",    b_gfx_open);
    eigs_register_function("gfx_close",   b_gfx_close);
    eigs_register_function("gfx_clear",   b_gfx_clear);
    eigs_register_function("gfx_rect",    b_gfx_rect);
    eigs_register_function("gfx_line",    b_gfx_line);
    eigs_register_function("gfx_point",   b_gfx_point);
    eigs_register_function("gfx_circle",  b_gfx_circle);
    eigs_register_function("gfx_text",    b_gfx_text);
    eigs_register_function("gfx_present", b_gfx_present);
    eigs_register_function("gfx_poll",    b_gfx_poll);
    eigs_register_function("gfx_ticks",   b_gfx_ticks);
    eigs_register_function("gfx_delay",   b_gfx_delay);
    eigs_register_function("gfx_title",   b_gfx_title);
    /* the pointer as a vital: sample mouse_x into a binding and the
     * observer reads the pointer's trajectory, same as heap_pressure */
    eigs_register_function("mouse_x",       b_mouse_x);
    eigs_register_function("mouse_y",       b_mouse_y);
    eigs_register_function("mouse_buttons", b_mouse_btn);
    /* MD: the WM's window seams (meaningful from the primary state) */
    eigs_register_function("task_view",  b_task_view);
    eigs_register_function("task_key",   b_task_key);
    eigs_register_function("task_wheel", b_task_wheel);
    eigs_register_function("task_eval",  b_task_eval);
    eigs_register_function("gfx_clip",   b_gfx_clip);
}

/* What a WINDOW task can call: draw (viewport-bound), read its size,
 * pop WM-forwarded keys, sample machine vitals. Registered into the
 * task's state at spawn — eigs_register_function binds into whichever
 * state is attached, which is the fresh task state at that point. */
static void register_task_side(void) {
    eigs_register_function("gfx_open",    b_gfx_open);
    eigs_register_function("gfx_close",   b_gfx_close);
    eigs_register_function("gfx_clear",   b_gfx_clear);
    eigs_register_function("gfx_rect",    b_gfx_rect);
    eigs_register_function("gfx_line",    b_gfx_line);
    eigs_register_function("gfx_point",   b_gfx_point);
    eigs_register_function("gfx_circle",  b_gfx_circle);
    eigs_register_function("gfx_text",    b_gfx_text);
    eigs_register_function("gfx_present", b_gfx_present);
    eigs_register_function("gfx_poll",    b_gfx_poll);   /* -> null in a task */
    eigs_register_function("gfx_ticks",   b_gfx_ticks);
    eigs_register_function("gfx_delay",   b_gfx_delay);
    eigs_register_function("gfx_title",   b_gfx_title);
    eigs_register_function("win_key",     b_win_key);
    eigs_register_function("win_wheel",   b_win_wheel);
    eigs_register_function("win_w",       b_win_w);
    eigs_register_function("win_h",       b_win_h);
    register_vitals();
    register_fs();      /* windows get the un-carved fs surface too */
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
    g_primary = st;
    register_tasks();
    serial_write("[m9] task builtins registered (spawn/step/read/alive/kill)\n");
    register_blk();
    serial_write("[m10] block builtins registered (blk_read/write locked)\n");

    /* M10: the fs daemon state — a dedicated EigsState running the
     * byte-unmodified regionfs module; the un-carved fs builtins
     * delegate into it. Created after the bundle provider so `import
     * regionfs` resolves from ROM. */
    g_fs_state = eigs_state_new();
    if (g_fs_state && eigs_thread_switch(g_fs_state)) {
        eigs_state_init_runtime(g_fs_state);
        register_blk();                       /* regionfs's substrate */
        EigsValue *fr = eigs_eval_string("import regionfs");
        if (fr) eigs_value_release(fr);
        if (eigs_has_error()) {
            eigs_clear_error();
            eigs_thread_switch(st);
            g_fs_state = 0;
            serial_write("[m10] fs daemon FAILED to load regionfs\n");
        } else {
            eigs_thread_switch(st);
            register_fs();
            serial_write("[m10] fs builtins registered "
                         "(read/write/rename over the regionfs state)\n");
            register_journal();
            serial_write("[m11] journal builtins registered "
                         "(trace tape -> the store)\n");
        }
    } else {
        g_fs_state = 0;
        serial_write("[m10] fs daemon state creation FAILED\n");
    }
    register_gfx();
    serial_write("[m9.5] gfx builtins registered (fb shadow + PS/2 pointer)\n");

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

    /* M9: cooperative tasks — spawn a worker state, step it three
     * times, read its progress back (numbers only cross states), kill
     * it. The scheduler-as-observer demo lives at the REPL. */
    EigsValue *v7 = eigs_eval_string(
        "tid is task_spawn of \"task_counter\"\n"
        "task_step of tid\n"
        "task_step of tid\n"
        "p is task_step of tid\n"
        "q is task_read of [tid, \"progress\"]\n"
        "task_kill of tid\n"
        "(p == 3) and (q == 3) and ((task_alive of tid) == 0)");
    int task_ok = v7 && eigs_value_type(v7) == EIGS_TYPE_NUM &&
                  (int)eigs_value_as_num(v7) == 1;
    if (v7) eigs_value_release(v7);
    if (eigs_has_error()) { task_ok = 0; eigs_clear_error(); }
    serial_write(task_ok ? "[m9] task spawn/step*3/read/kill = 3 OK\n"
                         : "[m9] task lifecycle self-test FAILED\n");
    vga_put_at(22, 0, task_ok ? "M9: task spawn/step/read/kill OK"
                              : "M9: task lifecycle FAILED",
               task_ok ? 0x0A : 0x4F);
    if (!task_ok) return 0;

    /* M9.5: pixels + pointer — the script draws through the second
     * backend, the kernel reads one pixel back from VRAM (the only
     * VRAM read anywhere: a self-test probe). On a text-console boot
     * (VBE declined) this is a documented skip, not a failure. */
    if (fb_active()) {
        EigsValue *v8 = eigs_eval_string(
            "g is gfx_open of [1024, 768, \"selftest\"]\n"
            "gfx_clear of [0, 0, 0]\n"
            "gfx_rect of [10, 10, 50, 50, 255, 64, 32]\n"
            "gfx_circle of [200, 40, 20, 32, 200, 64]\n"
            "gfx_line of [10, 100, 300, 100, 64, 64, 255]\n"
            "gfx_text of [10, 120, \"gfx on bare metal\", 220, 220, 220, 2]\n"
            "gfx_present of null\n"
            "g");
        int gfx_ok = v8 && eigs_value_type(v8) == EIGS_TYPE_NUM &&
                     (int)eigs_value_as_num(v8) == 1;
        if (v8) eigs_value_release(v8);
        if (eigs_has_error()) { gfx_ok = 0; eigs_clear_error(); }
        if (gfx_ok && fb_vram_pixel(20, 20) != fb_pack(255, 64, 32)) {
            serial_write("[m9.5] VRAM readback mismatch at (20,20)\n");
            gfx_ok = 0;
        }
        EigsValue *vc = eigs_eval_string("gfx_close of null");
        if (vc) eigs_value_release(vc);
        int ptr_ok = mouse_present();
        serial_write(gfx_ok ? "[m9.5] gfx rect/circle/line/text -> VRAM readback OK"
                            : "[m9.5] gfx self-test FAILED");
        serial_write(ptr_ok ? " (+ pointer)\n" : " (no pointer)\n");
        vga_put_at(23, 0, gfx_ok ? (ptr_ok ? "M9.5: gfx on framebuffer + mouse OK"
                                           : "M9.5: gfx on framebuffer OK (no mouse)")
                                 : "M9.5: gfx self-test FAILED",
                   gfx_ok ? 0x0A : 0x4F);
        if (!gfx_ok) return 0;
    } else {
        serial_write("[m9.5] gfx self-test skipped (VGA text console)\n");
        vga_put_at(23, 0, "M9.5: gfx skipped (text console)", 0x08);
    }

    /* m10: the storage stack, end to end — mount-by-magic probe (a
     * valid superblock is the write authorization; a foreign disk
     * refuses and stays locked). Read-only unless a store exists. */
    if (g_fs_state) {
        if (fs_ensure_mounted()) {
            EigsState *back = fsd_enter();
            double nf = fsd_eval_num("len of (regionfs.list_files of [])", -1);
            fsd_leave(back);
            serial_write("[m10] fs: regionfs mounted, ");
            serial_write_dec((uint64_t)(nf < 0 ? 0 : nf));
            serial_write(" file(s) OK\n");
        } else {
            serial_write(ata_present()
                             ? "[m10] fs: no store on disk (mount refused; writes stay locked)\n"
                             : "[m10] fs: no disk\n");
        }
    }

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
    /* journal_* verbs are META: neither their I record (jrn_record_input
     * skips it) nor their runtime L/A records belong in the ring — a
     * replay-only session must leave the ring untouched. */
    int jmeta = strncmp(src, "journal_", 8) == 0;
    if (jmeta && g_auto_on && !g_jrn_on) eigs_set_trace_sink(0, 0);
    jrn_record_input(src);        /* M11: the session journal's I record */
    EigsValue *v = eigs_eval_string(src);
    /* If the eval died with gfx holding the screen, take it back — an
     * error message must never render invisibly (the T3200 has no
     * serial to read it from). A CLEAN eval leaves gfx open on
     * purpose: hosted windows persist across lines; gfx_close of null
     * is the way back to the console. */
    if (eigs_has_error() && vga_suspended()) {
        g_gfx_open = 0;
        kbd_gfx_mode(0);
        vga_suspend(0);
    }
    if (eigs_has_error()) {
        con_write("error: ");
        con_write(eigs_last_error_message());
        con_putc('\n');
        eigs_clear_error();
    } else {
        repl_show_value(v);
    }
    if (v) eigs_value_release(v);
    if (jmeta && g_auto_on && !g_jrn_on) eigs_set_trace_sink(jrn_sink, 0);
    if (!stack_canary_ok()) {
        con_write("PANIC: boot-stack overflow (canary trampled by that "
                  "eval). Halting.\n");
        speaker_beeps(3);            /* 3 beeps = canary (2 = CPU exception) */
        for (;;) __asm__ volatile ("cli; hlt");
    }
}

/* M11 phase 2: drive a loaded session journal through the normal REPL
 * eval path. Runs in the REPL loop BETWEEN evals — the only legal spot
 * (an eval inside a builtin would nest the VM). Each replayed source
 * unit is echoed with reconstructed prompts, evaluated (its nondet
 * reads served from the tape in recorded order), then replay mode is
 * ended so live typing never consumes stale tape records. */
static void jrn_drive(void) {
    if (!g_rp_count) return;
    g_rp_driving = 1;                /* a replay must not record itself */
    if (g_auto_on) eigs_set_trace_sink(0, 0);   /* nor fill the ring */
    con_write("[journal] session replay:\n");
    for (int i = 0; i < g_rp_count; i++) {
        const char *src = g_rp_lines[i];
        con_write("eigs> ");
        for (const char *p = src; *p; p++) {
            if (*p == '\n') { if (p[1]) con_write("\n....> "); }
            else con_putc(*p);
        }
        con_putc('\n');
        repl_eval(src);
    }
    rp_clear();
    eigs_set_replay_tape(0, 0, 0);
    g_rp_driving = 0;
    if (g_auto_on) eigs_set_trace_sink(jrn_sink, 0);
    con_write("[journal] session replay complete (live readings resume)\n");
}

/* ---- the line editor (MD groundwork) ----
 * Arrows/home/end/delete + up/down history over the M6 block-input
 * REPL, with a visible inverse-video cursor (vga_cursor). Editing
 * renders to the CONSOLE only; serial — a log, not a terminal — gets
 * each line once, on submit, so the COM1 transcript stays faithful.
 * A line edits within one console row: the editable length is capped
 * at cols - prompt - 1 (78 on the fb console). */
#define ED_PROMPT 6                      /* "eigs> " and "....> " */
#define ED_HIST   8

static char g_hist[ED_HIST][512];
static int  g_hist_n, g_hist_next;

/* Repaint line[0..len) after the prompt, blank what the previous
 * render left behind, park the cursor at `cur`. */
static void ed_draw(const char *line, int len, int cur, int *shown) {
    vga_col_set(ED_PROMPT);
    for (int i = 0; i < len; i++) vga_putc(line[i]);
    for (int i = len; i < *shown; i++) vga_putc(' ');
    *shown = len;
    vga_col_set(ED_PROMPT + cur);
}

static void ed_prompt(const char *p) {
    con_write(p);                        /* serial log + console */
    vga_cursor(1);
}

void eigs_repl(void) {
    g_vga_on = 1;
    vga_clear();
    con_write("EigenScript REPL on EigenOS -- Enter evals. A line ending in ':'\n");
    con_write("opens a block: keep typing lines, a blank line runs it.\n");

    /* M11: the flight recorder — when a store mounted at boot, the
     * session journals itself; after a crash, journal_replay of
     * "autolog" re-drives what was lost. */
    if (g_fs_mounted) {
        /* archive the previous boot's ring FIRST — the first live flush
         * would otherwise overwrite the crash evidence */
        EigsState *back = fsd_enter();
        fsd_set_str("fsd_path", "autolog");
        if (fsd_eval_num("regionfs.file_exists of fsd_path", 0) == 1.0) {
            fsd_set_str("fsd_path2", "lastboot");
            fsd_eval_num("regionfs.rename of [fsd_path, fsd_path2]", 0);
            fsd_leave(back);
            con_write("[m11] auto-journal: previous session archived as \"lastboot\"\n");
        } else fsd_leave(back);
        g_auto_on = 1;
        eigs_set_trace_sink(jrn_sink, 0);
        con_write("[m11] auto-journal: on (ring \"autolog\" in the store)\n");
    }

    char line[512], draft[512];
    static char prog[4096];       /* accumulated block (M6 multi-line) */
    int len = 0, cur = 0, shown = 0;
    int draft_len = 0;
    int nav = -1;                 /* history offset; -1 = editing the draft */
    int plen = 0;                 /* accumulated block fill */
    int cont = 0;                 /* 1 = inside a ':'-opened block */
    ed_prompt("eigs> ");

    for (;;) {
        int c = kbd_getchar();
        if (c < 0) {
            vitals_sample_maybe();       /* M11: machine history, ~1 Hz */
            __asm__ volatile ("hlt");
            continue;                    /* wait for a key */
        }
        int maxlen = vga_cols() - ED_PROMPT - 1;
        if (maxlen > (int)sizeof(line) - 1) maxlen = (int)sizeof(line) - 1;

        if (c == '\n') {
            line[len] = 0;
            vga_cursor(0);
            vga_col_set(ED_PROMPT + len);
            vga_putc('\n');
            serial_write(line);           /* the log's copy of the line */
            serial_write("\n");
            if (len > 0 && (g_hist_n == 0 ||
                            strcmp(g_hist[(g_hist_next + ED_HIST - 1) % ED_HIST],
                                   line) != 0)) {
                memcpy(g_hist[g_hist_next], line, (size_t)len + 1);
                g_hist_next = (g_hist_next + 1) % ED_HIST;
                if (g_hist_n < ED_HIST) g_hist_n++;
            }
            nav = -1;
            /* Does this line open a block? Trailing ':' (spaces ignored). */
            int e = len;
            while (e > 0 && line[e - 1] == ' ') e--;
            int opens = (e > 0 && line[e - 1] == ':');

            if (!cont) {
                if (len == 0) { ed_prompt("eigs> "); continue; }
                if (opens) {                      /* start accumulating */
                    plen = 0;
                    for (int i = 0; i < len; i++) prog[plen++] = line[i];
                    prog[plen++] = '\n';
                    cont = 1;
                    ed_prompt("....> ");
                } else {                          /* single line: eval now */
                    repl_eval(line);
                    jrn_drive();                  /* M11: pending session replay */
                    jrn_auto_flush();             /* M11: ring -> the store */
                    ed_prompt("eigs> ");
                }
            } else {
                if (len == 0) {                   /* blank line: run block */
                    prog[plen] = 0;
                    cont = 0;
                    repl_eval(prog);
                    jrn_drive();
                    jrn_auto_flush();
                    ed_prompt("eigs> ");
                } else if (plen + len + 2 > (int)sizeof(prog)) {
                    con_write("error: block too large, discarded\n");
                    cont = 0;
                    ed_prompt("eigs> ");
                } else {                          /* keep accumulating */
                    for (int i = 0; i < len; i++) prog[plen++] = line[i];
                    prog[plen++] = '\n';
                    ed_prompt("....> ");
                }
            }
            len = cur = shown = 0;
        } else if (c == KEY_UP || c == KEY_DOWN) {
            if (g_hist_n == 0) continue;
            if (c == KEY_UP) {
                if (nav + 1 >= g_hist_n) continue;
                if (nav == -1) {                  /* park the in-progress line */
                    memcpy(draft, line, (size_t)len);
                    draft_len = len;
                }
                nav++;
            } else {
                if (nav == -1) continue;
                nav--;
            }
            if (nav == -1) {
                memcpy(line, draft, (size_t)draft_len);
                len = draft_len;
            } else {
                const char *h = g_hist[(g_hist_next + ED_HIST - 1 - nav) % ED_HIST];
                len = 0;
                while (h[len] && len < maxlen) { line[len] = h[len]; len++; }
            }
            cur = len;
            ed_draw(line, len, cur, &shown);
        } else if (c == KEY_LEFT) {
            if (cur > 0) { cur--; vga_col_set(ED_PROMPT + cur); }
        } else if (c == KEY_RIGHT) {
            if (cur < len) { cur++; vga_col_set(ED_PROMPT + cur); }
        } else if (c == KEY_HOME) {
            cur = 0; vga_col_set(ED_PROMPT);
        } else if (c == KEY_END) {
            cur = len; vga_col_set(ED_PROMPT + len);
        } else if (c == KEY_DELETE) {
            if (cur < len) {
                memmove(line + cur, line + cur + 1, (size_t)(len - cur - 1));
                len--;
                ed_draw(line, len, cur, &shown);
            }
        } else if (c == '\b') {
            if (cur > 0) {
                memmove(line + cur - 1, line + cur, (size_t)(len - cur));
                len--; cur--;
                ed_draw(line, len, cur, &shown);
            }
        } else if ((kbd_mods() & KMOD_CTRL) && (c == 'a' || c == 'e' ||
                                                c == 'u' || c == 'c')) {
            if (c == 'a') { cur = 0; vga_col_set(ED_PROMPT); }
            if (c == 'e') { cur = len; vga_col_set(ED_PROMPT + len); }
            if (c == 'u') { len = cur = 0; ed_draw(line, len, cur, &shown); }
            if (c == 'c') {                       /* cancel line AND block */
                vga_cursor(0);
                con_write("^C\n");
                len = cur = shown = 0;
                cont = 0; nav = -1;
                ed_prompt("eigs> ");
            }
        } else if (c >= 32 && c < 127) {
            if (len < maxlen) {
                memmove(line + cur + 1, line + cur, (size_t)(len - cur));
                line[cur] = (char)c;
                len++; cur++;
                if (cur == len && shown == len - 1) {
                    vga_putc((char)c);            /* fast path: append */
                    shown = len;
                } else {
                    ed_draw(line, len, cur, &shown);
                }
            }
        }
        /* everything else (ESC, pgup/pgdn) is ignored at the REPL */
    }
}
