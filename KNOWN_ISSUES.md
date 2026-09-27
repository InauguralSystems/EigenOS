# Known issues

(none open)

## RESOLVED — the kernel stopped linking against EigenScript main (EigenScript#1181)

**Symptom.** `make all` against current `../EigenScript` failed at the link
with about 50 undefined symbols (`builtin_buf_*`, `task_*`,
`is_builtin_name`, `register_host_builtins`, ...) plus `pthread_once` and
`pthread_self`. Separately, a bare `make` built nothing: the first rule in
the Makefile (`build/gen/tidefs.eigs`) was the default goal.

**Root cause (found 2026-09-27).** Two one-sided contracts.
1. `EIGS_SRCS` was a hand-typed copy of the runtime's source list. Upstream
   split the runtime into new units (`builtins_buf`, `builtins_host`,
   `fsutil`, `lint_host`, `task`), and the copy fell five behind. Nothing
   compared it with upstream. The Makefile was last touched 2026-07-21.
2. EigenScript's freestanding contract (`tools/freestanding_hal_roots.txt`)
   gained `pthread_once` (PRNG seeding) and `pthread_self` (#1142's
   replay-owner check). The upstream gate proves the runtime imports only
   listed roots, but nothing proved EigenOS defines them all. The allowlist
   comment asserted EigenOS "already owns" them; `hal.c` had 14 of 16.

**Fix.**
- `EIGS_SRCS` is derived from EigenScript's Makefile (`print-SOURCES` minus
  `print-CLI_ONLY`), the derivation upstream's own freestanding-check uses. An
  empty derivation is a build error.
- `hal.c` defines `pthread_once` (a flag plus a call) and `pthread_self`
  (a constant: one CPU thread; the replay check keys the owning state
  separately).
- `tools/hal_roots_check.sh` runs before every link and fails naming any
  listed root the kernel side does not define. Calibrated on four cases: the
  clean tree passes (32/32); main's `hal.c` fails naming both roots;
  removing only `pthread_self` fails through `make` before `ld`; an empty
  roots file fails as "examined nothing".
- `.DEFAULT_GOAL := all`.

Boot after the fix: every serial verdict OK through M13-2. At the REPL,
`print of ((random of []) != (random of []))` printed `1`, which is the
`pthread_once` path on bare metal.

## RESOLVED — M13-2: the journal recorded the full L-record trace, making heavy sessions unpersistable

**Symptom.** The M13-2 tape-replay gate (`tools/net_tape_replay.py`)
recorded a full ARP+DHCP+ICMP+UDP session, then `journal_off` never
printed its "written to the store" verdict — the write hung for minutes
and the session never persisted, so boot 2 had nothing to replay. The
M13-1 single-`arp_probe` tape had always written instantly.

**Root cause (found 2026-07-13).** The M11 journal sink stored EVERY
record the trace engine emits, including one `L <line>` record per
executed script line (`trace_line` in EigenScript `src/trace.c`). A
single `dhcp.acquire` crafts and checksums a 300-byte packet across tens
of thousands of executed lines, so the FULL trace ran ~57 KB for DHCP
alone; persisting it through the pure-EigenScript regionfs store under
TCG runs at ~400 B/s (~140 s), and a three-protocol session was several
times worse. The M11 replay path never needed the `L`/`A`/`S` records —
`journal_replay` re-drives the `I` (input) records and the replay-take
reader consumes only `N` (nondeterministic input) records plus the `V`
version header; everything else was dead weight.

**Fix (`src/hal.c`, `jrn_sink`).** The journal sink now streams
record-filtered: it keeps only `I`/`N`/`V` lines and drops the `L`/`A`/`S`
trace flood (first-byte-of-line classification, persisted across chunk
boundaries so a large multi-chunk `N` frame record is never split). The
same DHCP tape dropped 57 KB → 3.8 KB and its write 140 s → 12 s; the
full three-protocol session now persists in seconds and replays
byte-identically. A latent second bug fixed alongside: the auto ring's
newest-half slide was cutting the tape's leading `V` header (any
`autolog` larger than 64 KB would have been rejected as "no version
header" on crash replay) — the slide now runs only for the auto ring,
never a manual `journal_on..off` bracket, and the filtered records keep
even a full session comfortably inside the bound.

**Note (test-side, same milestone).** Byte-identical replay of the ICMP
leg also required `icmp.reset` (zero the cumulative `sent`/`got`/`rtt`/
`ping_seq`) and `inet.arp_flush` to be re-driven inside the recorded
session: the record boot's self-test warms the icmp counters and the
inet ARP cache, but the replay boot skips the icmp gate (no lease), so
without the reset the replayed ping sends a different sequence number
and the ip_send takes a different (cold-ARP) `net_recv` path, diverging
the tape. Live module state that a session's `net_recv` order depends on
must be reset inside the recorded session.

## RESOLVED — M5/MN: layout-sensitive crash in the freestanding runtime

**Symptom (historical).** After the first one or two `eigs_eval_string`
calls, a subsequent eval — a third eval, a function `define`, or an
observer loop — faulted with `#UD (invalid opcode)` at an address inside
`.rodata`: a corrupted opcode sent the VM's computed-goto dispatch into
data. The fault was deterministic per build but **moved with every
rebuild**, and survived zeroing both the kernel heap and the boot stack.

**Root cause (found 2026-07-02, fixed upstream in EigenScript PR #361).**
Not an uninitialized read — a **C stack overflow**. The EigenScript
compiler's `Compiler` struct embedded a 12 KiB `Local[512]` array by value,
and `compile_node_inner` stack-declares a `Compiler` for every nested
function: **~12.7 KiB of stack per AST level**. Compiling even the 5-deep
M5 print program overran the old **64 KiB** boot stack. Hosted (8 MiB
stacks + a guard page) never noticed; on bare metal there is no guard page,
so the overflow silently trampled the `.bss` placed below `stack_bottom`
and the corruption detonated on a *later* eval — wherever the current link
layout happened to put the victim global. That explains every observed
property: deterministic per build, moved with every rebuild, survived
heap/stack zeroing, invisible to the hosted suite and sanitizers.

**How it was found.** The uninit-read hypothesis was instrumented and
refuted first: the full hosted suite passed under `MALLOC_PERTURB_` and
under a new `make poison` build (0xAA-fill of xmalloc blocks, realloc
tails, and the env freelist's parked arrays), and a 104-step embed-API
REPL soak — the multi-eval-on-one-state pattern the suite never exercises —
ran clean under Valgrind memcheck in both the hosted and freestanding
profiles. The same soak re-run with `ulimit -s 64` (the boot-stack size)
segfaulted in `compile_node_inner`'s prologue at eval 2, and gdb measured
the ~12.7 KiB frames.

**The fix, both sides.**
- **Upstream (EigenScript PR #361):** the `locals` array is heap-allocated;
  `compile_node_inner`'s frame dropped 12,704 → 1,440 bytes. CI now runs
  `tools/embed_stack_soak.sh`, the REPL soak inside a 64 KiB stack rlimit.
- **EigenOS:** the boot stack grew 64 KiB → 1 MiB (worst-case compile
  stack at `COMPILE_MAX_DEPTH` is ~184 KiB post-fix), and `kernel_main`
  paints a **canary strip at `stack_bottom`** (`src/stackguard.h`) that the
  REPL checks after every eval — any future overflow panics loudly at the
  eval that caused it instead of leaving a layout-dependent time bomb.
  `eigs_run_demo` now runs the three former tripwires (third eval,
  define + call, observer loop) at every boot as the regression.

**The forcing-function lesson stands, sharpened.** The bare-metal port did
surface a real latent runtime bug invisible to the hosted gates — just in a
different resource class than diagnosed: the hosted suite bounds *what the
runtime computes*, but nothing hosted bounded *how much C stack it uses*.
EigenOS, with a 64 KiB stack, was the first consumer to apply that
constraint — and the fix (an 8.8× smaller compile frame) benefits every
embedder. The prove-outside/assert-inside pattern is now permanent: the
64 KiB-rlimit soak proves the bound hosted; the boot-stack canary asserts
it on the metal.
