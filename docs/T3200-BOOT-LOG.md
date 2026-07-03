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
