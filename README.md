# EigenOS

A bare-metal x86-64 operating system. The north star: run the
[EigenScript](https://github.com/InauguralSystems/EigenScript) runtime
**freestanding** — no host OS, no libc underneath it — where the kernel brings
up just enough hardware (memory, screen, keyboard, timer) and EigenScript is the
shell, userland, and everything else. It's the bare-metal counterpart to
"EigenScript is a single zero-dependency C binary," and a forcing function for
every hidden libc/syscall dependency in the runtime — surveyed in EigenScript's
[freestanding dependency ledger](https://github.com/InauguralSystems/EigenScript/blob/main/docs/FREESTANDING.md).

**Target hardware:** the Pentium Dual-Core T3200 (Penryn, x86-64) — the 2008
laptop freed from dev-baseline duty when development moved to the ASUS X540NA.
Developed and tested under QEMU first.

## Status

**Milestone-0 (done, boots):** GRUB (Multiboot2) loads the ELF64 kernel; the
boot stub transitions 32-bit protected mode → 64-bit long mode (identity-mapped
first 1 GiB with 2 MiB pages, PAE + LME + paging, 64-bit GDT), then `kernel_main`
runs in long mode and writes to VGA text memory. The stub hands the Multiboot2
info pointer (and magic) to `kernel_main`. Verified booting under QEMU.

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
Multiboot2 memory-map tag and builds a 4 KiB-page bitmap allocator over the
first 1 GiB (the range the boot stub identity-maps), reserving low memory
(< 1 MiB), the kernel image (`_kernel_start`/`_kernel_end` from the linker),
and the Multiboot2 info block. `pmm_alloc_page`/`pmm_free_page` are the
substrate of the `page_alloc` HAL root the EigenScript freestanding heap will
stand on (M5). An on-boot self-test (allocate → pattern-fill → verify → free,
free-count round-trip) prints its result to VGA; `make dump` reads it back
headless. RAM beyond 1 GiB is reported but not yet tracked (M3 brings virtual
memory).

## Build & run

```bash
make            # build build/EigenOS.elf (the ELF64 kernel)
make verify     # assert it's a valid Multiboot2 kernel (grub-file)
make iso        # build a bootable ISO via GRUB
make run        # boot the ISO in QEMU
make serial     # boot headless, stream the COM1 kernel log to the terminal
make dump       # boot headless, dump the VGA text screen out of the guest
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
- [ ] **M3** — paging/virtual memory, higher-half kernel
- [ ] **M4** — PS/2 keyboard, PIT timer
- [ ] **M5** — port the EigenScript runtime to freestanding (the forcing function).
  Scoped by the [freestanding dependency ledger](https://github.com/InauguralSystems/EigenScript/blob/main/docs/FREESTANDING.md):
  ~10 root primitives to implement against this kernel's HAL, the rest a
  write-once portable mini-libc/libm.
- [ ] **MN** — boot straight into an EigenScript REPL on the T3200

## License

MIT.
