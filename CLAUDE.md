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

### First visit: the M7 inventory (do this before anything else)

Run, and commit the raw output to the repo as `docs/T3200-INVENTORY.md`
(the dev side needs it):

```
sudo dmidecode -t system -t chassis      # exact model
grep -m1 flags /proc/cpuinfo             # MUST have ssse3; MUST LACK sse4_1/rdrand
sudo dmesg | grep -i "e820\|BIOS-provided" # the REAL memory map (PMM has only seen QEMU's)
sudo lspci -nn                           # NIC model (M13), FireWire, ExpressCard bridge
sudo cat /proc/tty/driver/serial         # uart:16550A at 0x3F8 = free serial console!
ip a                                     # working network -> scp delivery from the dev box
lsblk -f && findmnt /boot                # is /boot its own partition? (GRUB paths below)
```

Flag in the committed notes: any e820 reserved holes below 4 GiB (the PMM
must respect them), whether a real UART exists, and the NIC model.

### Build the kernel here (primary delivery path)

The portfolio was built on this machine for years; it can build its own OS:

```
mkdir -p ~/src/InauguralSystems && cd ~/src/InauguralSystems
gh repo clone InauguralSystems/EigenScript && gh repo clone InauguralSystems/EigenOS
cd EigenOS && make && make verify
```

(Alternative: `scp` a `build/EigenOS.elf` built on the dev box.)

### Dual-boot setup

```
sudo cp build/EigenOS.elf /boot/EigenOS.elf
sudo tee -a /etc/grub.d/40_custom >/dev/null <<'G'
menuentry "EigenOS" {
    set gfxpayload=text
    insmod multiboot2
    multiboot2 /boot/EigenOS.elf
    boot
}
G
sudo update-grub
```

Gotchas:
- If `findmnt /boot` shows a **separate /boot partition**, GRUB's path is
  `/EigenOS.elf`, not `/boot/EigenOS.elf`.
- Distro GRUB often hides the menu: ensure `/etc/default/grub` has
  `GRUB_TIMEOUT_STYLE=menu` and `GRUB_TIMEOUT=5`, then `update-grub` —
  otherwise you can't pick EigenOS (or get back to Linux easily).
- Reboot → pick EigenOS → expect the boot self-test verdicts on the VGA
  screen ending in the REPL. There is no serial on this laptop; the screen
  is the console. Power-cycle → pick Linux to come back.

### After a boot attempt

Record what happened (photo-transcribe the screen if needed) in
`docs/T3200-BOOT-LOG.md` and push: which `[mN]` verdicts printed, where it
stopped if it stopped, what a typed `6 * 7` did. If EigenOS hung: warm-
reboot into Linux — once the persistent-RAM crash log lands (M7 prep item,
NOT yet implemented) the log is read back here via /dev/mem; until then the
screen is the only report.

### The iteration loop once booted

Edit on either machine → `make` → `sudo cp build/EigenOS.elf /boot/` →
reboot → test → reboot to Linux. No media, no reflashing.
