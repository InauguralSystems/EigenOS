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

**Milestone-0 (done):** boots via Multiboot1, transitions 32-bit protected mode
→ 64-bit long mode (identity-mapped first 1 GiB, PAE + LME + paging), and writes
to VGA text memory. ~3 source files, gcc-only build.

## Build & run

```bash
make            # build build/EigenOS.elf  (needs only gcc + binutils)
make verify     # assert it's a valid Multiboot1 kernel (grub-file)
make run        # boot in QEMU            (needs qemu-system-x86_64)
```

QEMU is the only extra dependency to boot-test:
`sudo apt-get install -y qemu-system-x86`. No GRUB/ISO needed — QEMU loads the
multiboot ELF directly via `-kernel`.

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
