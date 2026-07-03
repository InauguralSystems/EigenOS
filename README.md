# EigenOS

A bare-metal x86-64 operating system whose shell, userland, and everything
else is [EigenScript](https://github.com/InauguralSystems/EigenScript). The
original north star — run the EigenScript runtime **freestanding**, no host
OS, no libc underneath it — was reached at M5, and the machine now boots
straight into an interactive EigenScript REPL (MN).

Like the language, the OS is **niche but general**: general purpose defines
the checklist (storage, multitasking, time, devices), the observer niche
defines how each item is answered — machine vitals as observed bindings, a
scheduler written as an observer loop, a crash-recoverable log as the
filesystem, the trace tape as a replayable system journal. Not POSIX
imitation. The full arc and its operating principles are in
[ROADMAP.md](ROADMAP.md).

Throughout, EigenOS is a **forcing function on the language** — each
subsystem stresses EigenScript on an axis no hosted repo can reach (it has
already surfaced the NaN-box low-canonical-malloc coupling and the compiler's
12.7 KiB-per-AST-level stack frame, both invisible to every hosted gate).
The runtime's external surface is surveyed in EigenScript's
[freestanding dependency ledger](https://github.com/InauguralSystems/EigenScript/blob/main/docs/FREESTANDING.md).

**Target hardware:** the Pentium Dual-Core T3200 (Penryn, x86-64) — the 2008
laptop freed from dev-baseline duty when development moved to the ASUS X540NA.
Developed and tested under QEMU first.

## Status

**Milestone-9.5 (done): pixels and a pointer.** The Multiboot2 header
requests a 1024x768x32 linear framebuffer (optional — GRUB boots us
either way); the console is now a cell **grid** (`src/vga.c`) rendered by
whichever backend the boot produced: 0xB8000 VGA text, or font-blit onto
the LFB (`src/fb.c`, 85x48 cells — the glyphs are EigenScript's own
`font5x7` from `ext_gfx.c`, extracted at build time by `tools/mkfont.py`,
so hosted windows and the bare-metal console draw identical text). All
gfx drawing goes to a RAM shadow buffer; `gfx_present` copies to VRAM
(VRAM is never read). EigenScript gets **ext_gfx's second backend**
(`src/hal.c`): `gfx_open/clear/rect/line/point/circle/text/present/
poll/ticks/delay` + `mouse_x/y/buttons`, with hosted-shaped event dicts —
a Tidepool-style `ev.type`/`ev.key` event loop runs unchanged; `import
draw` paints with the mouse at the bare-metal REPL. The pointer is a
PS/2 mouse driver on IRQ12 (`src/mouse.c`, i8042 aux). Gates: on-boot
self-test draws from script and reads the pixel back from VRAM (row 23);
screendump-verified painting; `core2duo -m 3840 -smp 2` green; the USB
dd-path green; **OVMF/EFI green — under EFI, GRUB hands a GOP
framebuffer and the same console renders, closing the old EFI-blindness
caveat**; `-vga none` degrades to the text console cleanly.

**Milestone-N (done): the interactive REPL.** The machine boots into an
EigenScript REPL — PS/2 keyboard in, VGA text console + serial out, one open
runtime so state persists across lines, value echo, error reporting
(`src/hal.c`, `src/vga.c`). The layout-sensitive `#UD` heisenbug that first
blocked it turned out to be a **boot-stack overflow**, not a runtime memory
bug: the EigenScript compiler cost ~12.7 KiB of C stack per AST level (fixed
upstream, [EigenScript PR #361](https://github.com/InauguralSystems/EigenScript/pull/361));
the boot stack is now 1 MiB and a canary strip at `stack_bottom`
(`src/stackguard.h`) is checked after every eval, so a future overflow
panics at the eval that caused it instead of trampling `.bss` silently.
Full post-mortem in [KNOWN_ISSUES.md](KNOWN_ISSUES.md). On-boot regression:
the three former tripwires (a third eval, `define` + call, an observer
loop) run at every boot, then assert the canary. Pre-hardware gates all
green in QEMU: boots under `-cpu core2duo -m 3840 -smp 2` (the T3200's
feature set and RAM — no post-Merom instructions in the image), and the
ISO is isohybrid and boots as a raw disk (the USB path).

**Milestone-0 (done, boots):** GRUB (Multiboot2) loads the ELF64 kernel; the
boot stub transitions 32-bit protected mode → 64-bit long mode (identity-mapped
first 1 GiB with 2 MiB pages, PAE + LME + paging, 64-bit GDT), then `kernel_main`
runs in long mode and writes to VGA text memory. The stub hands the Multiboot2
info pointer (and magic) to `kernel_main`. Verified booting under QEMU.

**Milestone-4 (done):** hardware interrupts — PIT timer + PS/2 keyboard.
The 8259 PIC is remapped off the exception vectors to 32-47 (`src/pic.c`),
the IDT gains IRQ gates, and `sti` turns interrupts on. The PIT
(`src/timer.c`) drives IRQ0 at 100 Hz into a monotonic tick counter (the
`clock` HAL root); a PS/2 keyboard driver (`src/keyboard.c`) turns IRQ1
scancodes into ASCII in a ring buffer (the `read_char` HAL root). These
are the last two HAL roots the freestanding runtime needs (M5). Proven
on boot: a timer self-test requires ticks to advance on their own, and
the kernel then echoes typed keys to serial — verified end-to-end by
injecting keystrokes through the QEMU monitor and reading them back off
COM1.

**Milestone-3 (done):** virtual memory. `paging_init` (`src/paging.c`)
builds fresh 4-level page tables — a low-1 GiB identity map so the
running kernel survives the `CR3` load, plus a **higher-half direct map
of all physical RAM** at `0xFFFF800000000000` — then switches `CR3`.
`map_page`/`unmap_page` do on-demand 4 KiB mappings, allocating
intermediate tables from the PMM. The direct map (`phys_to_virt`) lifts
M2's 1 GiB cap: the PMM now tracks all RAM (verified at 4 GiB —
~1.05M pages vs the old 262K), reachable through the map. A self-test
maps a page, writes through it, and confirms the same frame aliases via
the direct map. Relocating kernel *execution* into the higher half (to
free the low half for ring-3 userland) is deferred until a userland
milestone needs it. Verified via `make serial`.

**Milestone-1 (done):** IDT + CPU exceptions + serial console. A 16550
UART driver on COM1 (`src/serial.c`) becomes the headless logging seam —
the whole boot sequence streams to serial (`make serial`, or QEMU
`-serial`), no more VGA-memory dumps. An IDT (`src/idt.c` + `src/isr.S`)
installs handlers for all 32 CPU exception vectors with a uniform saved
frame; the dispatcher resumes cleanly from a breakpoint (`int3`, a trap)
and reports-then-halts on a genuine fault. Two on-boot self-tests prove
it: a UART loopback, and an `int3` that is caught and resumed. The GDT
was already set up by M0's boot stub, so M1 reuses it (a TSS/IST is
deferred until a ring-3 boundary needs a stack switch).

**Milestone-2 (done):** the physical memory manager. `pmm_init` walks the
Multiboot2 memory-map tag and builds a 4 KiB-page bitmap allocator over all
available RAM (a 64 GiB build cap; through M2 it was capped to the boot stub's
1 GiB identity map, lifted at M3 once the direct map made every frame
reachable), reserving low memory (< 1 MiB), the kernel image
(`_kernel_start`/`_kernel_end` from the linker), and the Multiboot2 info block.
`pmm_alloc_page`/`pmm_free_page` are the substrate of the `page_alloc` HAL root
the EigenScript freestanding heap will stand on (M5). An on-boot self-test
(allocate → pattern-fill → verify → free, free-count round-trip) prints its
result to VGA; `make dump` reads it back headless.

## Build & run

```bash
make            # build build/EigenOS.elf (the ELF64 kernel)
make verify     # assert it's a valid Multiboot2 kernel (grub-file)
make iso        # build a bootable ISO via GRUB
make run        # boot the ISO in QEMU
make serial     # boot headless, stream the COM1 kernel log to the terminal
make dump       # boot headless, dump the VGA text screen out of the guest
make shot       # boot headless, screendump the framebuffer -> build/screen.png
```

Build needs `gcc` + `binutils`. Booting needs `qemu-system-x86`, and `make iso`
needs `grub-pc-bin`/`grub-common` + `xorriso`:

```bash
sudo apt-get install -y qemu-system-x86 grub-pc-bin xorriso
```

The ISO is built **BIOS-only** (`grub-mkrescue -d /usr/lib/grub/i386-pc`), which
avoids the EFI image and its `mtools` dependency — QEMU boots BIOS (SeaBIOS) by
default. Earlier `qemu -kernel` is a dead end here: QEMU's Multiboot1 loader
rejects ELF64, so GRUB does the loading.

## Roadmap

- [x] **M0** — Multiboot → long mode → VGA text output
- [x] **M1** — IDT + CPU exception handlers, serial (COM1) for headless logging
- [x] **M2** — physical memory manager (parse the multiboot memory map)
- [x] **M3** — paging/virtual memory + higher-half direct map
- [x] **M4** — PS/2 keyboard, PIT timer (the read_char + clock HAL roots)
- [x] **M5** — the EigenScript runtime runs freestanding on EigenOS. The
  runtime + its `src/freestanding/` mini-libc/libm are compiled into the
  kernel image (`-DEIGENSCRIPT_FREESTANDING=1`, `-msse2`), and `src/hal.c`
  binds the profile's ~30 residual libc symbols to kernel services:
  `malloc` family → the kernel heap (`src/kheap.c`, over `page_alloc` +
  the M3 VMM), `printf`/`fprintf`/`putc`/`fflush`/`stdout`/`stderr` →
  serial, `clock_gettime`/`usleep` → the PIT, `exit`/`abort` → halt,
  `pthread_*` → single-thread stubs. SSE is enabled in the boot stub (the
  runtime is all doubles); `__thread` state is served by a variant-II TLS
  block with `%fs.base` set in `tls_init`. Verified over serial:
  `6 * 7 = 42`, a `for`/`range`/`str`/`print` program prints
  `sum 0..9 = 45`, and the **observer converges** (`converged=1`) —
  exercising `mini_libm`'s `log2` entropy formula on bare metal. The
  kernel keeps servicing keyboard/timer IRQs afterward.
  - Forcing-function finding: the runtime NaN-boxes heap pointers to 48
    bits, so `malloc` must return **low-canonical** addresses — the kernel
    heap lives at `0x0000100000000000`, not the higher half, or the first
    pointer deref is a non-canonical `#GP`.
- [x] **MN** — boot straight into an interactive EigenScript REPL (see
  Status above; the T3200 hardware boot itself is M7)

**Next** (the niche-but-general arc — full detail and rationale in
[ROADMAP.md](ROADMAP.md)):

- [x] **M6** — a REPL you can actually type into: full shifted-symbol
  keymap, multi-line block input (`:` opens, blank line runs — `define`
  and observer loops typed interactively), `gfxpayload=text`, guarded
  `make usb`. Verified via QEMU sendkey: `6*7`→42, typed `define`→144,
  typed observer loop→converged.
- [ ] **M7** — boot the T3200 (dual-boot via its resident Linux's GRUB;
  VGA + keyboard, no serial)
- [~] **MD** — the desktop, phase 1 done: windows are observed
  bindings — `import desktop` runs a window manager written in
  EigenScript (window = task = `EigsState`, viewport-clipped drawing,
  click focus + drag + grip-resize + z-order, key dispatch across
  states), a hung
  window's title turns red because `stable of` its progress trajectory
  says so, and the **REPL is a window**: lines typed into it evaluate
  on that window's own isolated `EigsState` (`task_eval`, strings by
  copy) — quit the desktop and its bindings are gone. The WM is
  list-driven: a window is a record in a z-ordered list, and adding
  one is a single `spawn_win` call (observer health stays in named
  scalar slots — a trajectory needs a binding, and list elements
  have none).
- [x] **M7.5** — the machine has software: the EigenScript stdlib (67
  modules) + demos baked into the image as a ROM bundle, served through
  the upstream source-provider seam (EigenScript PR #362) — `import
  math` works and `import life` runs Conway's Life at the bare-metal
  REPL, no filesystem anywhere.
- [x] **M8** — machine vitals as observed bindings: live kernel
  counters as builtins + the bundled `vitals` module; a quiet heap reads
  `stable`/`equilibrium` through the language's own observer — the
  machine observes itself at every boot (VGA row 21).
- [x] **M9** — multitasking: a task is an `EigsState`, a step is one
  eval, switched on one CPU thread via the upstream `eigs_thread_switch`
  seam (EigenScript PR #363); the scheduler is EigenScript and a stalled
  task's `progress` reads `stable` — scheduling policy as an observer
  predicate.
- [x] **M9.5** — pixels and a pointer: linear framebuffer + font-blit
  console (text fallback intact, EFI blindness fixed), ext_gfx's second
  backend for EigenScript, PS/2 mouse on IRQ12 — `import draw` paints
  on bare metal (see Status). Plus MD's input-dispatch groundwork:
  E0 keys (arrows/home/end/delete), keyup events + modifiers, the
  IntelliMouse wheel, and REPL line editing with a visible cursor and
  up/down history.
- [ ] **M10** — storage: tidelog-style crash-recoverable log as the
  filesystem, over an ATA driver
- [ ] **M11** — the system journal: trace tape over machine state;
  `prev of` / `state_at` against the computer itself; recovery = replay
- [ ] **M12** — devices as `.eigs` drivers (port I/O, MMIO, IRQ callbacks
  from the language)
- [ ] **M13** — networking (deliberately last)

## License

MIT.
