# T3200 boot log

Chronological record of boot attempts and Linux-side prep experiments on
the Gateway M-7317U target. See `T3200-INVENTORY.md` for the machine
snapshot.

## 2026-07-02 — Linux-to-Linux crash-log validation: BLOCKED by address collision

**Attempted:** the playbook's step-1 kernel-cmdline change to reserve the
crash-log physical region:
`GRUB_CMDLINE_LINUX_DEFAULT="quiet splash memmap=64K\$0x1000000 iomem=relaxed"`.
Escaping preserved cleanly through `update-grub` (verified: file still
shows the single-backslash form after edit).

**Result on this hardware:** the normal boot entry fails early with

```
Physical KASLR disabled: no suitable memory region!
```

and drops through into an unusable state (screen goes to recovery, no
usable login). Only the **recovery-mode** GRUB entry boots — but recovery
uses `GRUB_CMDLINE_LINUX` (plus fixed recovery flags), not
`GRUB_CMDLINE_LINUX_DEFAULT`, so `memmap` is not in effect and the
sentinel test cannot run in that state. Confirmed with
`cat /proc/cmdline` in recovery: `... recovery nomodeset dis_ucode_ldr`
with no `memmap=` flag.

**Root cause (analysis):** `memmap=64K$0x1000000` reserves 64 KiB starting
at physical `0x1000000` (16 MiB). That is exactly the default value of
`CONFIG_PHYSICAL_START` for x86-64 — the base of Linux's kernel image
load region. KASLR's slot search on this kernel treats the reservation as
"no suitable region" and bails. The choice of `0x1000000` for the EigenOS
crash-log ring collides with Linux's kernel base on the very host we need
to read the ring back from.

**Recovery from the target:** rebooted into GRUB recovery entry → root
shell → `mount -o remount,rw /` → edited `/etc/default/grub` back to
`GRUB_CMDLINE_LINUX_DEFAULT="quiet splash"` → `update-grub` → `reboot`.
Normal boot restored; `cat /proc/cmdline` shows
`... ro quiet splash vt.handoff=7`, no memmap.

**Blocker for the dev side:** the crash-log ring's physical address needs
to move out of Linux's kernel image range on this hardware. The T3200's
e820 (see `T3200-INVENTORY.md`) has plenty of usable RAM well above the
kernel image — anywhere in the `0x100000-0xbb6b0fff` region past the
kernel image is fine; `0x8000000` (128 MiB) is a comfortable choice.
Once the address moves in `src/crashlog.h` (and any PMM reservation
matches), the sentinel test can proceed.

**Firmware boot-menu / CSM check:** not yet done — held off until the
crash-log validation is unblocked so we don't reboot the machine twice.

## 2026-07-02 — dev-side response: crash-log ring moved to 0x4000000 (64 MiB)

The blocker above is fixed on `main`. `0x8000000` (the suggestion) was
rejected for a second collision: it is exactly the top of QEMU's default
`-m 128M`, which would put the ring outside RAM in every default-memory
QEMU gate run. `0x4000000` (64 MiB) dodges both: clear of
`CONFIG_PHYSICAL_START`/KASLR's default base, inside QEMU's default RAM,
inside the M-7317U's usable e820, and far above the kernel image.
Changed: `src/crashlog.h` (address + collision notes), the PMM
reservation follows the constant, `tools/t3200_read_crashlog.sh`, and
the CLAUDE.md setup snippet (now `memmap=64K\$0x4000000`, plus the
field-tested recovery procedure). Re-validated in QEMU: pmemsave at the
new address, reader output byte-faithful vs the serial log; T3200 sim
gate green. **Target side: re-run the one-time setup with the NEW
address, then the sentinel test + the firmware CSM check.**

## 2026-07-02 — target retry at 0x4000000: ALSO fails (decompressor hang)

Applied the new address on the target (cmdline verified
`memmap=64K\$0x4000000 iomem=relaxed`, escaping preserved, update-grub
clean). Same failure signature as `0x1000000`:

- Screen prints `Physical KASLR disabled: no suitable memory region!`,
  then **solid black forever** — sat 15 minutes, no further output, no
  panic banner, no reboot loop, hard-reset required.
- Recovery entry still boots (recovery uses `GRUB_CMDLINE_LINUX`, not
  `_DEFAULT`, so memmap is inactive there; same as before).
- Field-tested recovery procedure used again to revert to
  `"quiet splash"`; target is back on a clean cmdline.

**Diagnosis (best guess):** the KASLR "no suitable region" line is a
red herring — the fatal event is the kernel **decompressor hanging**
after KASLR gives up and falls back to the default load address. On
Ubuntu 24.04 (kernel 6.8.0-124), the decompressed image plus early boot
regions likely span well past `0x4000000` starting from
`CONFIG_PHYSICAL_START=0x1000000`. Reserving 64 KiB at 64 MiB lands
inside the decompressor's target zone, and either it can't relocate
around the hole or it partially writes into it. Boot dies silently
right at the jump into the decompressed kernel — before init, before
any userspace, before anything hits the journal (confirmed:
`journalctl -b -1` shows the *previous* clean shutdown, not this
attempt — the failed boot never wrote anything).

**Blocker for the dev side:** the crash-log ring needs to live **well
above the whole decompressed kernel image + initrd** on this hardware,
not just above `CONFIG_PHYSICAL_START`. Candidates that clear both:

- `0x10000000` (256 MiB) — probably safe on all sane Ubuntu installs.
- `0x40000000` (1 GiB) — definitely safe on this 3.8 GiB machine, still
  well inside `-m 3840` QEMU gate. Cheapest to justify.
- Above 4 GiB (`0x100000000+`) — cleanest / no collision possible, but
  needs Multiboot2 to hand a 64-bit address in the memory map and
  early-boot to use 64-bit addressing before the direct map is up.
  Probably overkill for M7.

Recommend `0x40000000`. Retry once the address moves in `src/crashlog.h`
+ playbook + reader script.

Firmware boot-menu / CSM check still deferred (same reasoning as before —
avoid double reboots while the crash-log path is unblocked).

## 2026-07-02 — dev-side response: ring moved to 0x3FFF0000 (1 GiB − 64 KiB)

`0x40000000` (the recommendation) has a collision only visible from the
dev side: it is one byte PAST EigenOS's boot-stub identity map (M0 maps
exactly 1 GiB), and `crashlog_init` runs as kernel_main's first act —
before M3 paging — so the ring must be identity-mapped from the first
instruction. `0x10000000` was rejected as only "probably" clear of the
decompressor zone, and a third bricked-boot trip is worth more than 192
MiB of margin. `0x3FFF0000` — the last 64 KiB under 1 GiB — satisfies
everything at once: inside the identity map (ring ends exactly at
0x40000000), far above any decompressor zone, inside the M-7317U usable
e820, and RAM-backed in QEMU at `-m 1024` (all Makefile QEMU targets now
pass `-m 1024` so the ring stays live in every dev run). Full collision
ledger now lives in `src/crashlog.h`. Re-validated: pmemsave at the new
address byte-faithful vs serial; core2duo/3840 gate green.

**Target side next visit: one-time setup with `memmap=64K\$0x3fff0000
iomem=relaxed`, sentinel warm-reset test, firmware CSM check.**

## 2026-07-02 — THIRD black screen at 0x3FFF0000 → dev-side rediagnosis: it was never the address

Three wildly different addresses (16 MiB / 64 MiB / 959 MiB) produced
the IDENTICAL failure. The only thing common to all three attempts is
the option itself — and there's a double-expansion escaping bug that
explains everything:

1. `/etc/default/grub` is **sourced by `sh`** during `update-grub`.
   Under `sh`, `"...\$..."` collapses to a literal `$`.
2. The generated `/boot/grub/grub.cfg` then contains
   `memmap=64K$0x3fff0000` — and **GRUB expands `$0x3fff0000` as a
   variable** when parsing the menuentry. Undefined → empty.
3. The kernel actually received **`memmap=64K`** = "use only 64 KiB of
   RAM" → KASLR "no suitable memory region" → decompressor death →
   black screen. Same result for ANY address, because the address never
   reached the kernel.

Both earlier diagnoses ("kernel base collision", "decompressor zone")
were plausible, written down confidently, and **never verified at the
point of truth**: no failing boot ever had its `/proc/cmdline`
inspected — the only entries that booted were recovery entries, which
don't carry `_DEFAULT`. Same lesson as the mn-repl heisenbug, one layer
down: instrument the diagnosis before acting on it.

### Procedure for THIS visit (after the usual recovery + revert)

1. **Smoking-gun check (no reboot):** if any failing config remains,
   `grep -n memmap /boot/grub/grub.cfg` — expect the UNESCAPED
   `memmap=64K$0x3fff0000` (no backslash) = GRUB ate the address.
2. **One-time proof boot (no permanent change, no revert risk):** at
   the GRUB menu press `e` on the normal entry, append to the `linux`
   line — single backslash, the GRUB editor has one expansion layer:
   `memmap=64K\$0x3fff0000 iomem=relaxed`
   then Ctrl-X. On success **VERIFY FIRST**:
   `cat /proc/cmdline` must show `memmap=64K$0x3fff0000` with the full
   address.
3. **Sentinel warm-reset test** (as in CLAUDE.md): write the DRAMTEST
   string via /dev/mem at `0x3fff0000`, `sudo reboot` (warm), read it
   back. Sentinel survives → the crash-log channel is proven.
4. **`sudo cat /proc/iomem` and paste it here** — the measured physical
   map (kernel code/data/bss, initrd, our region as `reserved`). This
   is the no-guessing oracle for any future address decision; it also
   tells us whether the OLD diagnoses had any truth in them.
5. **Make it permanent — TRIPLE backslash** in `/etc/default/grub`:
   `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash memmap=64K\\\$0x3fff0000 iomem=relaxed"`
   then `sudo update-grub`, and BEFORE rebooting verify the generated
   config: `grep memmap /boot/grub/grub.cfg` must show `\$0x3fff0000`
   WITH the backslash. Missing backslash = do not reboot.
6. Firmware CSM/boot-menu check on one of these reboots, as before.

Record every outcome here (incl. a failed step 2 — that would refute
the escaping theory and make the address story live again).

## 2026-07-02 — Full sentinel run: escaping fixed, ring works, DRAM does NOT persist

Two important results from this visit, one procedural, one hardware.

### Escaping ladder: end-to-end verified

Executed the dev-side procedure exactly:

1. Confirmed `/etc/default/grub` was clean (plain `"quiet splash"` — the
   earlier successful WRITE boot had been the one-time GRUB-editor route,
   which reverts on the next boot).
2. Edited `/etc/default/grub` via `sudo nano` to
   `GRUB_CMDLINE_LINUX_DEFAULT="quiet splash memmap=64K\\\$0x3fff0000 iomem=relaxed"`
   (three backslashes before `$`).
3. `sudo update-grub` → `sudo grep memmap /boot/grub/grub.cfg` showed
   `memmap=64K\$0x3fff0000 iomem=relaxed` — **one** backslash, exactly
   as the theory predicted. That's the safety gate we're now committing
   to as the permanent verification step.
4. Rebooted. Post-boot `cat /proc/cmdline`:
   `... ro quiet splash memmap=64K$0x3fff0000 iomem=relaxed vt.handoff=7`
   — literal `$`, full address, made it to the kernel intact for the
   first permanent (not one-time) configuration on this hardware.
5. `/proc/iomem` shows the reservation applied:
   `00100000-3ffeffff : System RAM` / `3fff0000-3fffffff : Reserved` /
   `40000000-bb6b0fff : System RAM`. Clean 64 KiB carve-out, exactly at
   the requested address.
6. `/dev/mem` read at `0x3fff0000` returned data (no EPERM):
   `ffff ffff fefd f7df ffff f7ff bfff bfff` — floating/uninitialized
   DRAM. `iomem=relaxed` is confirmed active for System RAM here.

So the crash-log ring **setup mechanism** is fully validated on the
T3200: reservation, escaping, devmem access, all work. The dev-side
playbook's step 2 procedure is byte-for-byte correct.

Bonus KASLR datum for the "kernel base collision" and "decompressor
zone" theories: kernel code was at `0xb3e00000` (~2.9 GiB) in one boot
and `0x11800000` (~280 MiB) in the next — KASLR is randomizing freely
across the full range. Neither address is anywhere near `0x1000000`,
`0x4000000`, or `0x3fff0000`. Both theories can be retired for good;
the escaping bug was the sole cause of all three black screens.

### Sentinel warm-reset test: FAILED — DRAM does NOT survive warm reset here

Ran the sentinel test as documented:

```
echo "DRAMTEST-$(date +%s)" | sudo dd of=/dev/mem bs=1 \
    seek=$((0x3fff0000)) conv=notrunc status=none
# pre-reboot readback (sanity):
sudo dd if=/dev/mem bs=1 skip=$((0x3fff0000)) count=32 status=none | cat -v
# → "DRAMTEST-1783042409\n" + floating DRAM tail — write landed clean.
sudo reboot
# after login (cmdline still had memmap+iomem — verified):
sudo dd if=/dev/mem bs=1 skip=$((0x3fff0000)) count=32 status=none | cat -v
# → no DRAMTEST string, floating-DRAM pattern only:
#   M-^?M-^?M-^?M-^?M-~M-}M-wM-_M-^?M-^?M-wM-^?M-?M-^?M-?M-^?...
```

The post-reboot bytes match the pre-write bytes closely (both are
"mostly 0xff with sparse bit drops at 0xfe/0xfd/0xf7/0xdf/0xbf/etc.").
That's a DRAM-decay pattern (individual cells losing charge at different
rates), NOT a BIOS zero-fill. Interpretation: on this hardware, warm
reset either lets the memory controller reinitialize DRAM or drops
refresh for long enough that cells discharge.

**Design implication for M7:** the persistent-RAM crash-log ring at a
fixed physical address relies on DRAM surviving warm reset. **That
assumption is invalid on the T3200.** Every setup piece (reservation,
escaping, devmem) works — the underlying hardware behavior doesn't
cooperate. The channel is dead for this machine.

Alternatives the dev side should weigh (none decided here):

1. **VGA-text panic screen as the sole post-mortem channel.** The
   playbook already renders `[mN]` verdicts to VGA; make panic dump
   go there too. Photograph the screen. Cheap, matches the "no serial"
   reality on this hardware, and doesn't depend on cooperative DRAM.
2. Check firmware Setup (F2) for a "Quick Boot" / "No Memory Test" /
   "Resume from S3" / "Memory Preserve" toggle — some 2008-era BIOSes
   have a memory-preserve option that only activates in specific paths.
   Deferred to the F2 firmware pass below.
3. CMOS/NVRAM (256 bytes) for a panic-reason code only — enough for
   "which subsystem" but not the full ring. Small change to the ring
   design (bank-select byte + reason code).
4. kexec-style soft reset from EigenOS eventually (avoids
   BIOS/memory-controller reset entirely) — way past M7.
5. Direct disk write (SATA driver) — driver complexity budget, but
   most reliable. Also way past M7.

Recommendation: for M7 first boot, accept ephemeral crash log. VGA is
the only live console anyway (all four ISA UARTs `uart:unknown`),
so the screen is the post-mortem — matching option 1. Revisit ring
persistence if the F2 pass finds a memory-preserve toggle.

### Firmware fact worth flagging: BIOS/CSM, not UEFI

`/sys/firmware/efi` does not exist on this system;
`mokutil --sb-state` responds `EFI variables are not supported on this
system`. Combined: **Ubuntu is currently booting this machine via CSM /
legacy BIOS**, not UEFI, even though `/boot/efi` is mounted (ESP exists
from the install; it just isn't the active boot path).

That contradicts the current CLAUDE.md inventory summary ("firmware is
UEFI"). The full-machine story is: firmware **supports** UEFI (there IS
a signed ESP) but currently boots **legacy**. For M7 that's a
simplification, not a complication:

- The USB-via-CSM primary attempt path in the playbook is *already the
  active boot mode* on this machine. No firmware toggle needed to try
  it — just F10 to the boot menu, pick the USB.
- The "runs blind under GOP" concern for the EFI menuentry path is
  moot for the first attempt: BIOS boot = real VGA text mode from the
  start.
- Suggest updating the CLAUDE.md T3200 section to say "firmware
  supports UEFI, currently boots via CSM" and adjusting the EFI/GOP
  caveat accordingly.

Firmware Setup / CSM toggle check still deferred (this session
consumed all the reboot budget on the sentinel test; F2 pass to be
done on the next reboot).

### Blocking-item digest for the dev side

- **Escaping procedure works permanently** — commit the triple-backslash
  form to CLAUDE.md as canonical; the one-time GRUB-editor form is now
  only the sanity check, not the setup step.
- **DRAM warm-reset persistence: FALSE on this hardware.** Persistent
  crash-log ring design needs re-thinking. My recommendation is option
  (1) above (VGA-only post-mortem), but the design call is yours.
- **Boot mode is BIOS/CSM**, not UEFI — first-attempt path is
  unblocked without any firmware setup pass.
