# CLAUDE.md — EigenOS working guide

EigenOS is a bare-metal x86-64 OS whose shell/userland is the EigenScript
runtime, freestanding. **Niche but general**: general purpose defines the
subsystem checklist, the observer niche defines how each item is answered —
no POSIX imitation. Vision + milestone arc (M6 ✓ → M7 T3200 boot → M8
vitals-as-observed-bindings → M9 multitasking → M10 tidelog-FS → M11
journal → M12 .eigs drivers → M13 net): **ROADMAP.md**. Resolved-bug
post-mortems: **KNOWN_ISSUES.md**.

**First: figure out which machine you are on** (`hostnamectl` /
`grep -m1 "model name" /proc/cpuinfo`):
- A **Pentium Dual-Core T3200** → you are on the TARGET. Read the
  "On the T3200" section below; it overrides the rest.
- Anything else (the ASUS X540NA / N3350 is the usual dev box) → you are
  on the dev side; QEMU is the target until M7 lands.

## Build & run (dev side)

```
make            # build/EigenOS.elf (Multiboot2 ELF64; GRUB loads it)
make verify     # grub-file Multiboot2 check
make iso        # BIOS-only bootable ISO (isohybrid — dd-able to USB)
make serial     # boot headless in QEMU, stream COM1 to the terminal
make dump       # boot headless, read the VGA text screen via monitor xp
make usb USB=/dev/sdX   # write the ISO to a stick (guarded; overwrites!)
```

- The build **couples to the sibling `../EigenScript` checkout** (no pin):
  the runtime + mini-libc compile into the kernel image. Check the sibling
  is clean/expected before building.
- **SSE split is load-bearing**: kernel objects are `-mno-sse`; only the
  runtime/mini-libc/hal.c build with SSE. Nothing SSE-compiled may be
  reachable from IRQ context (the ISR trampoline saves GP regs only).
- Every milestone lands with **on-boot self-tests** printing PASS/FAIL
  verdicts to serial + VGA (bounded busy-waits — a dead subsystem FAILS,
  never hangs), README status + ROADMAP checkboxes updated in the same
  commit.
- T3200 simulation gates before any hardware claim: boot
  `-cpu core2duo -m 3840 -smp 2`; image must contain zero post-Merom
  instructions (`objdump -d | grep -cE "rdrand|ptest|popcnt|crc32"` = 0).
- Language gaps found here are fixed **upstream in EigenScript** (branch →
  PR → merge), never worked around. Deep procedure (fault triage ladder,
  sendkey driving, invariants): the `eigenos-kernel-engineer` skill.

## On the T3200 (the target machine)

You are on the target hardware. It runs Linux and **stays** running Linux —
EigenOS dual-boots via the existing GRUB. Linux here is the recovery path
and the post-mortem debugger. Hard rules:

- **Never** repartition, `dd` to internal disks, or touch the Linux install
  beyond `/boot` and GRUB config. Dual-boot only.
- This is a ~3.8 GiB machine: plain `make`, one heavy job at a time.
- To push results you need `gh auth login` (or an SSH key) on this box.

### Inventory: DONE (docs/T3200-INVENTORY.md, 2026-07-02) — don't re-run

The load-bearing facts: Gateway M-7317U, GM45/ICH9M; CPU matches the sim
gate exactly; fragmented e820 (PMM verified hole-safe); **no serial** (all
four ISA UARTs probe `unknown` — the screen is the only live console);
**firmware is UEFI** (Ubuntu boots grub-efi from the ESP on sda2); /boot is
NOT a separate partition; Wi-Fi works (scp/gh fine); the disk logs ATA
exceptions — don't depend on it beyond /boot.

**UEFI status (verified on the dev side under OVMF, 2026-07-02):** the
kernel boots green under GRUB-EFI — handoff, fragmented memory map, all
self-tests, REPL. But UEFI leaves the display in a GOP graphics mode, so
VGA text at 0xB8000 does NOT render: booted via the EFI menuentry, EigenOS
runs blind (and there's no serial here). Hence the attempt order below.

### Build the kernel here (primary delivery path)

The portfolio was built on this machine for years; it can build its own OS:

```
mkdir -p ~/src/InauguralSystems && cd ~/src/InauguralSystems
gh repo clone InauguralSystems/EigenScript && gh repo clone InauguralSystems/EigenOS
cd EigenOS && make && make verify
```

(Alternative: `scp` a `build/EigenOS.elf` built on the dev box.)

### Boot attempt order (M7)

**1. USB stick via the firmware's legacy/CSM boot path — the primary
attempt.** A 2008 EFI almost certainly ships CSM. Build the stick
(`make usb USB=/dev/sdX` — on THIS machine only use a removable stick,
never the internal disk), reboot into the firmware boot menu (usually F12
or F10 on Gateways; check Setup for a "Legacy"/"CSM"/"Boot mode" toggle
and note what you find in docs/T3200-BOOT-LOG.md). Pick the USB in legacy
mode → BIOS GRUB → real VGA text mode → self-test verdicts on screen →
the REPL. This is the path where everything works as already designed.

**2. The EFI GRUB menuentry (dual-boot, no media) — only useful once a
framebuffer console exists.** Proven under OVMF to boot and run fine, but
the screen stays in GOP graphics mode: the REPL runs invisibly and this
laptop has no serial. Do not use this path for the first attempt; it
becomes the everyday path after the framebuffer console lands. For
reference when that day comes:

```
sudo cp build/EigenOS.elf /boot/EigenOS.elf
sudo tee -a /etc/grub.d/40_custom >/dev/null <<'G'
menuentry "EigenOS" {
    insmod multiboot2
    multiboot2 /boot/EigenOS.elf
    boot
}
G
sudo update-grub    # also set GRUB_TIMEOUT_STYLE=menu, GRUB_TIMEOUT=5
```

(`/boot` is not a separate partition here, so the GRUB path really is
`/boot/EigenOS.elf`.)

### One-time Linux setup for the crash log (do BEFORE the first attempt)

EigenOS mirrors every log byte into a persistent-RAM ring at phys
`0x3fff0000` (64 KiB, survives warm reset). For Linux to read it back after
a hang, add to `GRUB_CMDLINE_LINUX_DEFAULT` in `/etc/default/grub`:

```
memmap=64K\$0x3fff0000 iomem=relaxed
```

The `\$` is REQUIRED (grub eats a bare `$`). Then `sudo update-grub`,
reboot, and verify `cat /proc/cmdline` shows `memmap=64K$0x3fff0000`.
`memmap` keeps Linux out of the buffer; `iomem=relaxed` unblocks /dev/mem
(Ubuntu ships STRICT_DEVMEM).

The address (1 GiB − 64 KiB) is the survivor of two field failures and
two dev-side vetoes — see `src/crashlog.h` for the full collision list:
16 MiB = Linux's kernel load base (no boot), 64 MiB = inside the kernel
decompressor's zone (black-screen hang), 128 MiB = top of QEMU's default
RAM, 1 GiB exactly = one byte past EigenOS's boot identity map. Do NOT
"simplify" it. If a memmap change ever bricks the normal entry again:
GRUB recovery entry → root shell → `mount -o remount,rw /` → revert
`/etc/default/grub` → `update-grub` → reboot. Field-tested, twice.

**Then validate the whole channel end-to-end — no EigenOS needed.** The
one open question is whether this firmware preserves DRAM across a warm
reset. Prove it Linux→Linux (ONLY after the cmdline reboot above — before
it, Linux may be using that page):

```
echo "DRAMTEST-$(date +%s)" | sudo dd of=/dev/mem bs=1 \
    seek=$((0x3fff0000)) conv=notrunc status=none
sudo reboot        # warm reset — no power cycle
# after logging back in:
sudo dd if=/dev/mem bs=1 skip=$((0x3fff0000)) count=32 status=none | cat -v
```

Sentinel survives → the crash-log channel is proven on this hardware;
record the result (either way) in `docs/T3200-BOOT-LOG.md` and push. If
it does NOT survive, say so loudly in the log — the post-mortem plan
falls back to the VGA screen + beep codes only.

**While rebooting anyway:** enter the firmware setup / boot menu once and
note in `docs/T3200-BOOT-LOG.md` what it offers — a "Legacy"/"CSM"/"Boot
mode" toggle and the boot-menu key (F12/F10?). That answers which path
the first EigenOS boot uses (see "Boot attempt order").

### After a boot attempt

Record what happened in `docs/T3200-BOOT-LOG.md` and push: which verdicts
showed on screen (the `[mN]` board rows + the MN rows 17–19), where it
stopped if it stopped, what a typed `6 * 7` did, and any beeps (2 = fatal
CPU exception, 3 = stack-canary panic). If EigenOS hung or the screen was
dead: warm-reboot into Linux and run

```
sudo tools/t3200_read_crashlog.sh
```

— it prints the full boot log back out of RAM (validated in QEMU:
byte-faithful vs the serial log). Note: booting EigenOS again overwrites
the previous log; read it from Linux first.

### The iteration loop once booted

Edit on either machine → `make` → `sudo cp build/EigenOS.elf /boot/` →
reboot → test → reboot to Linux. No media, no reflashing.
