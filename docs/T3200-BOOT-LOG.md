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
