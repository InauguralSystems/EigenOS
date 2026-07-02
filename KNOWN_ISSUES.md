# Known issues

(none open)

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
