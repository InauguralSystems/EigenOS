# CLAUDE.md — EigenOS working guide

EigenOS is a bare-metal x86-64 OS whose shell/userland is the EigenScript
runtime, freestanding. **Niche but general**: general purpose defines the
subsystem checklist, the observer niche defines how each item is answered —
no POSIX imitation. Vision + milestone arc (M6 ✓ → M7 T3200 boot (open) → M8
vitals-as-observed-bindings ✓ → M9 multitasking ✓ → M10 tidelog-FS ✓ →
M11 journal ✓ → M12 .eigs drivers ✓ → M13-1 e1000 frame seam ✓ →
M13-2 protocol tower in script ✓ (ARP/IPv4/ICMP/UDP/DHCP; the
TCP-vs-ext_http decision deliberately open)): **ROADMAP.md**.
Resolved-bug
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
                # (text-fallback boots only — fb boots need `make shot`)
make shot       # boot headless, screendump the framebuffer -> build/screen.png
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
**boots via BIOS/CSM, NOT UEFI** (`/sys/firmware/efi` absent; an ESP
exists from the Ubuntu install but is not the active boot path — the
earlier "firmware is UEFI" conclusion was an unverified inference from
the mounted /boot/efi); /boot is NOT a separate partition; Wi-Fi works
(scp/gh fine); the disk logs ATA exceptions — don't depend on it beyond
/boot. **DRAM does NOT survive warm reset here** (sentinel-tested
2026-07-02) — no RAM-persistence tricks on this hardware.

**UEFI insurance (dev side, under OVMF):** the kernel is proven to boot
green under GRUB-EFI too — but under EFI the display stays in GOP
graphics mode (no VGA text). Irrelevant while this machine boots CSM;
matters only if the boot mode ever changes or on future hardware.

### Build the kernel here (primary delivery path)

The portfolio was built on this machine for years; it can build its own OS:

```
mkdir -p ~/src/InauguralSystems && cd ~/src/InauguralSystems
gh repo clone InauguralSystems/EigenScript && gh repo clone InauguralSystems/EigenOS
cd EigenOS && make && make verify
```

(Alternative: `scp` a `build/EigenOS.elf` built on the dev box.)

### Boot attempt order (M7)

**1. USB stick — the primary attempt, and it needs NO firmware changes:
this machine already boots BIOS/CSM natively.** Build the stick
(`make usb USB=/dev/sdX` — on THIS machine only use a removable stick,
never the internal disk), **F10** at power-on for the boot menu (Setup
is **F2**), pick the USB → BIOS GRUB → self-test verdicts on screen →
the REPL. Since M9.5 the normal on-screen mode is the **1024x768
font-blit framebuffer console** (GRUB/VBE honors the kernel's MB2
request; GM45 VBE should oblige); if VBE declines the mode, the kernel
falls back to real VGA text on its own — SAME board, same verdicts,
either way. `import draw` at the REPL is the pointer proof (the
touchpad is PS/2 aux → IRQ12).

**2. The GRUB menuentry (dual-boot, no media) — the everyday path once
the stick has proven the kernel on this hardware.** Since the machine
boots BIOS GRUB (not EFI), the GOP-blindness concern does NOT apply
here: a menuentry boot gets real VGA text exactly like the stick. For
when that day comes:

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

### Post-mortem channels (updated after the 2026-07-02 sentinel run)

**The RAM crash-log ring does NOT work as a reboot-surviving channel on
this hardware**: DRAM provably does not survive warm reset here (the
sentinel test wrote a string at `0x3fff0000`, verified it, rebooted, and
read back only floating-DRAM decay). Every setup piece was validated —
the triple-backslash `memmap` escaping, the reservation
(`/proc/iomem` shows `3fff0000-3fffffff : Reserved`), `iomem=relaxed`
devmem reads — but the physics vetoes the design on this machine.

Consequences:
- **The post-mortem channels on the T3200 are the VGA screen and the
  beep codes** (2 = fatal CPU exception, 3 = stack-canary panic). Panic
  verdicts render on screen; photograph it.
- **The Linux-side memmap/iomem cmdline is NOT needed — revert to a
  clean `"quiet splash"`** if it's still set. (The escaping procedure
  and the field-tested GRUB recovery live in docs/T3200-BOOT-LOG.md if
  a memmap= is ever needed again; the generated-config `grep` check
  before rebooting is mandatory.)
- The ring itself stays in the kernel: it costs nothing, and on the dev
  side it IS the post-mortem channel (QEMU `pmemsave` + the reader
  script read the live/paused guest — no reboot involved).
- Possible future revival: the F2 firmware pass may find a
  memory-preserve / no-memory-test toggle; a CMOS panic-reason byte is
  a cheap alternative for "which subsystem died".

### After a boot attempt

Record what happened in `docs/T3200-BOOT-LOG.md` and push: which verdicts
showed on screen (the `[mN]` board rows + the MN rows 17–19), where it
stopped if it stopped, what a typed `6 * 7` did, and any beeps (2 = fatal
CPU exception, 3 = stack-canary panic). Photograph the screen for the
log — it is the only record this hardware keeps (DRAM does not survive
the reset; see "Post-mortem channels").

### The iteration loop once booted

Edit on either machine → `make` → `sudo cp build/EigenOS.elf /boot/` →
reboot → test → reboot to Linux. No media, no reflashing.

## This design is pre-v1 and mostly written by older models — question it

**90% or more of this repo, and of EigenScript and the AOT it runs on, was
designed by Claude sessions running models many generations old.** Newer
models keep arriving that are substantially stronger, the ecosystem is
**pre-v1**, and there are no external consumers to break. A decision you
find in the tree — here or upstream — carries **no authority from
seniority**.

That applies in both directions, and the second one is the point of a
consumer repo. When you hit a rough edge in the runtime, the standing rule
is already to surface it as an upstream issue rather than work around it
silently. Add to that: ask whether the thing you hit is a **law** of the
language or an **earlier decision**. The tell is writing, or thinking,
*"X must be true because the runtime does Y."*

Bought 2026-08-28 (ouroboros#127 / DMG). The AOT then compiled a program's
main file but emitted `load_file` as a runtime call, so loaded modules were
interpreted by the linked VM (since fixed, ouroboros#129 — the reasoning is
the lesson, not the state). A real bug in that seam was found, minimised,
fixed and verified — and reported as "unlocking the AOT multiplier for
DMG". Measured on being challenged: DMG is 3,288 lines, 818 compiled and
2,470 interpreted, including the 128-function opcode dispatch. Every
emulated instruction ran interpreted, so the fix made it *run* and could not
make it *faster*. A whole investigation cycle had treated that design as
terrain, and the capability to do it the other way already existed upstream
for another purpose.
