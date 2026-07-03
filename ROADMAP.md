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
   Oberon lineage). Its finished form is **the desktop (MD)** — general
   purpose in the personal-computer sense: a screen with your things on
   it, several alive at once, all of it written in EigenScript.
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

- [x] **M6 — a REPL you can actually type into (DONE 2026-07-02).**
      Full shifted-symbol keymap (`src/keyboard.c`, `kmap_shift`);
      multi-line input in `eigs_repl` (a line ending in `:` opens a
      block that accumulates; a blank line submits — so `define`, `for`,
      and observer loops are typed interactively); `set gfxpayload=text`
      in grub.cfg; `make usb USB=/dev/sdX` (guarded — refuses to guess a
      device). Verified end-to-end in QEMU via monitor `sendkey`:
      `a is 6*7`→42, string concat with `"`/`+`/`!`, a typed multi-line
      `define` (`sq of 12`→144), a typed observer loop (`converged`→1),
      list literals via brackets; T3200 sim gate re-run green.
- [ ] **M7 — boot the T3200.** The crossing from emulated to real.
      The target is a **Gateway M-7317U** (inventory: docs/T3200-INVENTORY.md;
      boot experiments: docs/T3200-BOOT-LOG.md): GM45/ICH9M, CPU flags
      match the sim gate exactly, ~3.94 GiB fragmented e820 (PMM verified
      hole-safe), Wi-Fi works, **no serial**, no FireWire/ExpressCard,
      flaky disk. **Boots via BIOS/CSM natively** (/sys/firmware/efi
      absent — the earlier "UEFI" conclusion was an unverified inference;
      an inactive ESP exists). **DRAM does NOT survive warm reset**
      (sentinel-tested), so the RAM crash-log ring is a dev-side/QEMU
      instrument only; on this hardware the post-mortem channels are the
      VGA screen + panic beep codes (2 = CPU exception, 3 = canary), and
      all boot/demo verdicts render to VGA (rows 17–19 + the board).
      Attempt plan (simplified by the CSM finding — no firmware changes
      needed, real VGA text is the native mode):
      1. **USB stick** (`make usb USB=/dev/sdX`, F10 boot menu at
         power-on, Setup = F2) → BIOS GRUB → VGA text → verdicts on
         screen → the REPL.
      2. Once proven: the **GRUB menuentry dual-boot** becomes the
         everyday no-media path — BIOS GRUB gives the same VGA text (the
         GOP-blindness caveat applied only to an EFI boot path).
      3. UEFI insurance already banked: the kernel boots green under
         OVMF+GRUB-EFI (matters for future hardware / if the boot mode
         ever changes; a framebuffer console would be needed for video
         in that mode).
      Success = the M5/MN self-tests green on screen and a hand-typed
      observer loop converging on 2008 silicon.
      Later upgrade (only when live debugging earns it): a gdb remote
      stub — but with no UART on this machine it would ride the M13 NIC,
      so it moves behind networking. USB-to-USB is a dead end (both
      machines are USB hosts; a bridge cable would need a full USB host
      stack in EigenOS).

- [x] **M7.5 — the machine has software (DONE 2026-07-03).** The
      upstream **source-provider seam** landed (EigenScript PR #362:
      `eigs_set_source_provider` — `import` consults the provider first
      in every profile; the only module source freestanding; M10's real
      filesystem later re-backs the same seam). EigenOS side: the **ROM
      bundle** — `tools/mkbundle.py` bakes the entire EigenScript stdlib
      (67 modules) + `demos/life.eigs` (~505 KiB of source) into the
      image as `.rodata` byte arrays; `hal.c` registers the bundle as
      the provider at eigs_open. Boot self-test: `import math` →
      `abs(-7)=7` verdict on VGA row 20. Verified in QEMU via sendkey:
      `import math`, `math.abs of -7` → 7, and **`import life` runs
      Conway's Game of Life on the console** — a glider crossing a
      toroidal grid, observer report on the population — real software
      on a machine with no filesystem. core2duo/3840 gate green.
      (Also fixed upstream: build.sh had drifted from the Makefile —
      missing eigs_embed.c — the known drift trap, caught by the matrix
      CI jobs.)

## Phase 3 — the self-observing computer

- [x] **M8 — machine vitals as observed bindings (DONE 2026-07-03).**
      Six live kernel counters exposed as builtins from hal.c
      (`heap_used`, `heap_mapped`, `mem_free_pages`, `mem_total_pages`,
      `ticks`, `tick_hz`) + a bundled `vitals` module wrapping them
      (`heap_pressure`, `mem_free_ratio`, `uptime_s`, `watch_heap`).
      The forcing-yield question answered itself cleanly: NO new
      observer semantics were needed — sample the vital into an ordinary
      binding in a loop and the binding's observer slot accumulates the
      trajectory; the vital IS just a value. Boot self-test (VGA row
      21): 14 samples of heap_pressure → `stable of hp` = 1 — the
      machine observes itself at every boot. REPL-verified via sendkey:
      the typed loop, `stable of hp` → 1, `report of hp` → equilibrium,
      `vitals.uptime_s of []` → seconds since boot. A leak now reads as
      `improving`/`diverging` on heap_pressure long before it is a
      crash. core2duo/3840 gate green.
- [x] **M9 — multitasking (DONE 2026-07-03).** The niche answer held:
      **a task IS an `EigsState`** — its own global env, module cache,
      observer thresholds — and a step is one eval on it. The upstream
      seam is `eigs_thread_switch` (EigenScript PR #363): parks the
      calling thread's attachment with NO teardown and activates another
      state's, all on the single CPU thread; the arena/freelists/VM live
      on the EigsThread and survive the switch. hal.c exposes
      `task_spawn/step/read/alive/kill` (a task program is a bundle
      module with a `step()` fn); only numbers cross states. The
      **scheduler is EigenScript** (`demos/sched.eigs` round-robins;
      policy is the observer): a task whose `progress` reads `stable` is
      stalled. Boot self-test (VGA row 22): spawn task_counter, step ×3,
      read progress = 3, kill. REPL-verified via sendkey: two tasks
      (task_counter, task_stuck) stepped in a loop — `stable of pa` = 0
      (healthy, advancing), `stable of pb` = 1 (stalled) — scheduling
      policy as an observer predicate, live on the console. Cost a real
      bug: the first cut matched attachments by `&eigs_current`, which
      miscompiles under the hand-rolled TLS → every switch re-attached →
      16 MiB arena leak per step → OOM; fixed to identify by state (PR
      #363). core2duo/3840 gate green.

- [ ] **M9.5 — pixels and a pointer.** The Multiboot2 framebuffer tag
      gets GRUB/VBE to hand over a linear framebuffer (works on the
      BIOS/CSM path AND fixes the EFI-blindness case in one move — the
      old "framebuffer console" item generalizes into this). Font blit +
      fill/blit/line primitives exposed to EigenScript as freestanding
      HAL roots (`ext_gfx`'s second backend). PS/2 mouse driver (IRQ12,
      sibling of the keyboard driver — the T3200 touchpad is PS/2 aux).
- [ ] **MD — the desktop.** The general-purpose capstone: EigenOS as a
      personal computer in the full sense, and the niche answer to it —
      **windows are observed bindings**. A window system written in
      EigenScript over M9 tasks (window = task = `EigsState`) and M9.5
      pixels: launcher, the REPL as a first-class window, the M8 vitals
      monitor as the first widget. Every desktop ever built bolts on a
      task manager afterward to ask "what is the machine doing?" — here
      a hung app IS `diverging`, a settled one IS `converged`, for free.
      Forcing yield: real-time input-event dispatch across EigsStates,
      the gfx HAL under sustained load (Tidepool's frame-limiter lessons
      apply), and whatever a window manager written in an observer
      language turns out to need.

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
