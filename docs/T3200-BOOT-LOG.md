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
