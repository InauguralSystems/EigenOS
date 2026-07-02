# Known issues

## M5/MN: layout-sensitive crash in the freestanding runtime (open)

**Symptom.** Running the EigenScript runtime freestanding (M5) works for the
first one or two `eigs_eval_string` calls — `6 * 7 = 42`, a `for`/`range`/
`str`/`print` program (`sum 0..9 = 45`), and (at some layouts) list/string
programs and the observer loop all succeed. But a subsequent eval — a third
`eigs_eval_string`, a function `define`, or an observer loop — intermittently
faults with `#UD (invalid opcode)` at an address inside `.rodata`. The M1
exception handler reports it over serial; the faulting `rip` is a wild VM
dispatch target (a corrupted opcode indexes past the computed-goto table).

**Why it's a heisenbug.** The fault is **deterministic per build but moves
with every rebuild** — adding or removing unrelated code shifts the link
layout and changes which eval faults (or whether it faults at all). It
survives zeroing both the kernel heap (`kmalloc`) and the boot stack
(`kernel_main`), so it is not simply reused-dirty memory.

**Diagnosis.** This is the signature of an **uninitialized-memory read** in
the runtime: hosted glibc hands back fresh, zeroed pages, so an uninitialized
field reads as a benign `0`; bare metal (different garbage, different layout)
reads a value that, interpreted as a pointer/opcode, sends the VM into data.
It is exactly the bug class the runtime's gates do **not** cover — ASan and
UBSan catch out-of-bounds and UB, but **MSan** (uninitialized reads) is
deferred upstream (needs an instrumented libc + arena annotations). EigenOS,
by giving the runtime a non-glibc substrate, is acting as an MSan-style
differential and has surfaced a real latent bug.

**This is the headline forcing-function finding of EigenOS.** The bare-metal
port did its job: it found a class of runtime bug invisible to the hosted
test suite (2365/2365, ASan/UBSan clean).

**Next steps (a focused session, not a quick patch).**
1. Run the freestanding runtime under **MSan** hosted (build the mini-libc +
   runtime with `-fsanitize=memory`) to name the uninitialized read directly.
2. Or attach **gdb to the QEMU stub** (`-s -S`), break in `vm_run`'s dispatch,
   and catch the step where the opcode/`ip` goes wild — walk back to the
   struct field that was never initialized.
3. Fix upstream in EigenScript (initialize the field), add a regression, and
   the freestanding profile + EigenOS boot-to-REPL become deterministic.

**What works today.** The kernel (M0–M4), the heap, TLS, SSE, the HAL glue,
the VGA console, and the REPL are all correct and verified. The REPL boots
and is interactive (assignments, variable persistence across lines, value
echo, error reporting) until the accumulated runtime state trips the bug
above. This is tracked on the `mn-repl` branch; `main` stays at the verified
M5 commit until the runtime bug is fixed.
