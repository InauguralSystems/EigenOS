# EigenOS roadmap

## What this OS is

EigenOS is **niche but general**, the same shape as the language it exists
for. EigenScript's niche (observer semantics — free, total measurement of
every assignment) was never a limit on what it can do; the consumer repos
prove generality axis by axis (an emulator, a SAT solver, a compiler, a
transformer), each written in the observer idiom. The OS inherits that
structure:

- **General purpose defines the checklist**: storage, multitasking, time,
  devices, eventually networking. A computer you can actually use.
- **The niche defines how each item is answered**: in the observer-native
  idiom, not by imitating POSIX. Files-as-byte-streams and fork/exec are
  Unix's answers to Unix's questions; EigenOS answers the same needs with
  the primitives EigenScript already proved hosted — observed bindings,
  temporal queries, durable determinism.

The end state, in three layers (each useful if we stop there):

1. **The EigenScript machine** — boot → REPL → `define` → the machine does
   it. One language as shell, userland, and services; no impedance layer
   between using the computer and programming it (the Lisp-machine /
   Oberon lineage).
2. **The self-observing computer** — the observer extended from program
   bindings to *machine* bindings: heap pressure, tick jitter, IRQ rates
   as observed values, so `converged of heap_pressure` is a kernel-policy
   primitive, not a monitoring query. Every other OS bolts measurement on
   afterward and pays for it; here it is the substrate.
3. **The replayable machine** — the trace tape as system journal:
   `prev of` / `state_at` against machine state, crash recovery as log
   replay. The composition of what liferaft (deterministic replay under
   chaos) and tidelog (crash-recoverable durable determinism) already
   proved hosted, applied to the whole computer.

And throughout: EigenOS is a **forcing function on the language**. Every
subsystem stresses EigenScript on an axis no hosted repo can reach — it has
already found the NaN-box low-canonical-malloc coupling and the
12.7 KiB-per-AST-level compiler stack frame, both invisible to every hosted
gate. When a milestone hits a language gap, the gap is surfaced and fixed
upstream, never worked around.

## Operating principles

- **Forcing-function ordering.** Build next whatever has the highest
  language-gap yield, not what Unix history says comes next.
- **No POSIX imitation.** Before adding a Unix-shaped interface, ask what
  the EigenScript-native answer is.
- **Small, serially verified milestones.** Every milestone lands with
  on-boot self-tests observable over serial (`make serial`), the way
  M0–MN did.
- **Prove outside, assert inside.** Invariants get a hosted proof
  (EigenScript CI: the embed REPL soak under a 64 KiB stack rlimit, the
  freestanding symbol gate, the mini-libc glibc differential) and a
  bare-metal assert (the boot-stack canary, on-boot self-tests). Hosted
  Linux is the only place sanitizers exist; use it for everything it can
  see before trusting the metal.
- **Target-hardware gates run in QEMU first.** `-cpu core2duo -m 3840
  -smp 2` approximates the T3200 (Penryn: SSSE3, no SSE4, no RDRAND) —
  the kernel must stay clean of post-Merom instructions, boot at 3.8 GiB,
  and boot from the ISO attached as a raw disk (the USB path; the ISO is
  isohybrid). All three verified 2026-07-02.

## Phase 1 — the substrate (DONE)

- [x] **M0** boot: GRUB/Multiboot2 → long mode → VGA text
- [x] **M1** IDT + CPU exceptions + COM1 serial (the headless logging seam)
- [x] **M2** physical memory manager over the Multiboot2 memory map
- [x] **M3** virtual memory: 4-level paging + higher-half direct map
- [x] **M4** hardware interrupts: 8259 PIC + PIT timer + PS/2 keyboard
- [x] **M5** the EigenScript runtime runs freestanding (mini-libc/libm +
      HAL glue; kernel heap; SSE; TLS)
- [x] **MN** boot into an interactive EigenScript REPL (VGA console +
      keyboard; state persists across lines). The `#UD` heisenbug that
      blocked it was a **boot-stack overflow** (the compiler cost
      ~12.7 KiB of C stack per AST level — fixed upstream, EigenScript
      PR #361); the boot stack is now 1 MiB with a canary strip at
      `stack_bottom` checked after every eval. See KNOWN_ISSUES.md for
      the full post-mortem.

## Phase 2 — the EigenScript machine

- [ ] **M6 — a REPL you can actually type into.** The keymap shift-maps
      letters only: `* + " ( ) [ ]` cannot be typed, which rules out
      multiplication, strings, lists, and calls. And input is one logical
      line per Enter, so `define` and observer loops cannot be entered
      interactively at all. Work: full shifted-symbol keymap
      (`src/keyboard.c`); multi-line continuation in `eigs_repl` (a line
      ending in `:` or continuing an indented block accumulates; a blank
      line submits); verified end-to-end in QEMU via monitor `sendkey`.
      Also banked here (already measured, small): `set gfxpayload=text`
      in grub.cfg (kill the works-in-QEMU/black-screen-on-real-BIOS
      class) and a `make usb` target (the ISO is already isohybrid —
      document/wrap the `dd`).
- [ ] **M7 — boot the T3200.** The crossing from emulated to real. The
      T3200 currently runs Linux — **dual-boot via its existing GRUB, not
      a wipe**: copy `build/EigenOS.elf` into its `/boot`, add a
      menuentry (`multiboot2 /boot/EigenOS.elf` + `set gfxpayload=text`),
      reboot and pick it. The iteration loop is copy → reboot → test →
      reboot back to Linux (scp delivery if its network works; GRUB's
      menu is the recovery path). The USB stick (`make usb`; the ISO is
      already isohybrid) stays as the fallback/portable path.
      Prep, in order:
      1. **Inventory under the resident Linux** (no live USB needed):
         `lspci -nn` (NIC for M13; FireWire; ExpressCard bridge),
         `/proc/tty/driver/serial` (a hidden onboard 16550 = free serial
         console), `/proc/cpuinfo` flags (confirm ssse3, no sse4/rdrand),
         `dmesg | grep e820` (the REAL memory map — the PMM has only ever
         seen QEMU's clean one), `dmidecode` (exact model).
      2. **Persistent-RAM crash log**: EigenOS logs to a reserved
         physical buffer with a magic header; after a hang, warm-reboot
         into Linux and read it back via `/dev/mem`. The resident Linux
         is the post-mortem debugger — no serial hardware needed.
         Validate once that this BIOS preserves DRAM across warm reset.
         Plus panic beep/LED codes for when even VGA is dead.
      3. Expect VGA + keyboard only live (no COM1 unless the inventory
         finds one) — audit that the on-boot self-tests render verdicts
         to VGA before the first attempt.
      Success = the M5/MN self-tests green on screen and a hand-typed
      observer loop converging on 2008 silicon.
      Later upgrade (only when live debugging earns it): ExpressCard
      16550 + minimal PCI enumeration → serial console from the ASUS
      (USB-serial dongle + null modem) → a gdb remote stub over that
      link for true remote debugging. USB-to-USB is a dead end (both
      machines are USB hosts; a bridge cable would need a full USB host
      stack in EigenOS).

## Phase 3 — the self-observing computer

- [ ] **M8 — machine vitals as observed bindings.** The first genuinely
      new thing: expose kernel vitals (`heap_pressure` from kheap's live
      byte count, tick jitter, IRQ rates, PMM free pages) as EigenScript
      bindings observed like any other value, so the existing predicates
      apply to the machine itself: `converged of heap_pressure`,
      `diverging of tick_jitter`. Small (kheap already counts bytes) but
      it is the moment EigenOS stops being "an OS that runs EigenScript"
      and becomes the OS EigenScript implies. Forcing yield: how observer
      slots bind to values produced by C-side sampling rather than
      assignment.
- [ ] **M9 — multitasking.** The general need: run two things at once.
      The niche answer: a task is an `EigsState` (the multi-state
      embedding API exists — docs/EMBEDDING.md upstream), scheduled
      cooperatively first, and the scheduler is an observer loop over
      task vitals — scheduling policy written in the language's own
      predicates. **Highest language-gap yield on the roadmap**: the
      freestanding profile currently stubs all 15 pthread symbols to
      single-thread no-ops, so this forces the real story for the
      scheduler HAL roots (spawn/yield/park/unpark + CAS from the
      FREESTANDING ledger) and for what spawn/channel mean without
      pthreads underneath.

## Phase 4 — the durable, replayable machine

- [ ] **M10 — storage: tidelog as the filesystem.** The general need:
      files that survive a reboot. The niche answer: a log-structured,
      crash-recoverable store with time-travel reads (tidelog proved the
      whole design hosted — CBOR codec, every-offset crash sweep,
      compaction with atomic swap) sitting on a block driver (ATA PIO
      first; the T3200 has a plain SATA spinner). Un-carve the
      freestanding profile's filesystem builtins (`read_text`,
      `write_bytes`, `load_file`/`import`, the package tree) onto it.
      Forcing yield: the durability primitives (#248–250) re-proven with
      no OS underneath; what `import` means when the "filesystem" is a
      replayable log.
- [ ] **M11 — the system journal.** The trace tape as machine journal:
      `prev of` and `state_at` against system state, boot-to-now history
      as a first-class object, crash recovery as replay rather than fsck
      archaeology. Forcing yield: the trace-tape file sinks are carved
      out of the freestanding profile today; they come back targeting
      M10's store, and determinism gets pressure-tested with no host
      clock underneath (the PIT is the only time source).

## Phase 5 — outward

- [ ] **M12 — devices in EigenScript.** Drivers as `.eigs` programs:
      port I/O, MMIO/volatile semantics, interrupt-driven callbacks from
      the language. Proof: reimplement the PS/2 keyboard driver in
      EigenScript and swap it in. The newest axis the portfolio has never
      touched — likely the richest untapped gap-finder (bytes, volatile,
      FFI, IRQ-safe callbacks).
- [ ] **M13 — networking (later).** A NIC driver + the HTTP extension
      freestanding. Deliberately last: biggest surface, lowest
      forcing-function novelty (the HTTP stack is already hardened
      hosted).

## Non-goals

- POSIX compatibility, or any compatibility layer.
- Multi-user, security domains, or ring-3 isolation before a milestone
  actually needs them (the kernel/task boundary is the runtime itself
  for now).
- Adoption. EigenOS's consumers are its author and the language it
  sharpens.
