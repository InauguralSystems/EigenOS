# EigenOS

A bare-metal x86-64 operating system. The north star: run the
[EigenScript](https://github.com/InauguralSystems/EigenScript) runtime
**freestanding** — no host OS, no libc underneath it — where the kernel brings
up just enough hardware (memory, screen, keyboard, timer) and EigenScript is the
shell, userland, and everything else. It's the bare-metal counterpart to
"EigenScript is a single zero-dependency C binary," and a forcing function for
every hidden libc/syscall dependency in the runtime.

**Target hardware:** the Pentium Dual-Core T3200 (Penryn, x86-64) — the 2008
laptop freed from dev-baseline duty when development moved to the ASUS X540NA.
Developed and tested under QEMU first.

## Status

**Milestone-0 (done, boots):** GRUB (Multiboot2) loads the ELF64 kernel; the
boot stub transitions 32-bit protected mode → 64-bit long mode (identity-mapped
first 1 GiB with 2 MiB pages, PAE + LME + paging, 64-bit GDT), then `kernel_main`
runs in long mode and writes to VGA text memory. Verified booting under QEMU.

## Build & run

```bash
make            # build build/EigenOS.elf (the ELF64 kernel)
make verify     # assert it's a valid Multiboot2 kernel (grub-file)
make iso        # build a bootable ISO via GRUB
make run        # boot the ISO in QEMU
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
- [ ] **M1** — GDT/IDT, CPU exception handlers, serial (COM1) for headless logging
- [ ] **M2** — physical memory manager (parse the multiboot memory map)
- [ ] **M3** — paging/virtual memory, higher-half kernel
- [ ] **M4** — PS/2 keyboard, PIT timer
- [ ] **M5** — port the EigenScript runtime to freestanding (the forcing function)
- [ ] **MN** — boot straight into an EigenScript REPL on the T3200

## License

MIT.
