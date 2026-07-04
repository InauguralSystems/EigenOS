# T3200 boot day — the M7 runbook

One page for the day the stick arrives. The deep procedure lives in
CLAUDE.md ("On the T3200") and docs/T3200-BOOT-LOG.md; this is the
checklist. The machine boots BIOS/CSM natively (verified — no firmware
changes needed), has NO serial, and its screen is the only console:
**photograph everything**.

## 1. Pre-flight (dev box, before touching the stick)

```
git -C ../EigenScript pull --ff-only     # sibling builds into the image
make && make verify && make iso
make serial   # sanity: watch the verdict ladder go green, ctrl-a x to quit
```

Expected serial ladder (all must be OK / documented-skip):
m0 multiboot → m1 serial+IDT → m2 pmm → m3 paging → m4 timer →
m5 kheap + evals → mn define/observer/canary → m7.5 import →
m8 vitals → m9 tasks → m9.5 gfx (+pointer) → m10 ata/fs probe →
m11 auto-journal → m12 pci vendor / irq loopback / kbd.eigs /
break(ctrl-c) / rtc stamp / spkr gate / cpu_margin.

Write the stick (dev box, never on the T3200 with its internal disk):

```
lsblk                          # identify the stick — triple-check
make usb USB=/dev/sdX          # guarded, but it OVERWRITES /dev/sdX
```

## 2. Boot attempt

Power on → **F10** (boot menu; Setup is F2) → pick the USB stick →
BIOS GRUB → EigenOS.

- **Normal outcome**: 1024x768 font-blit framebuffer console, the
  verdict board rows green, then the REPL.
- **VBE declines the mode** (e.g. offers only 24 bpp): kernel falls
  back to real VGA text on its own — same board, same verdicts, still
  a PASS (the whole ladder is green on a text boot; gfx row reads
  "skipped"). Note which console mode came up.
- **cpu_margin should show a real number** here (QEMU can't) — first
  reading of the machine's own die temperature.
- **No store on the internal disk is CORRECT**: mount-by-magic refuses
  the Linux MBR disk and writes stay locked. "[m10] fs: no store
  (mount refused; writes stay locked)" is the expected, safe verdict.
- Beeps: 2 = fatal exception, 3 = stack-canary panic. Photograph the
  screen before power-cycling — DRAM does not survive reset here.

## 3. The proof script (type at the REPL, photograph each)

```
6 * 7                      # the machine computes
rtc.stamp of []            # ...wait: import rtc first
```

In order, with imports:

```
import rtc
rtc.stamp of []            # the machine knows what time it is (UTC)
import spkr
spkr.chime of []           # the machine SPEAKS (real speaker at last)
cpu_margin of []           # die temp margin — real DTS reading
import pci
pci.scan of []             # enumerate the GM45 board from script
import life                # Conway on the panel (native speed, not TCG)
```

Keyboard driver swap-in (typing through EigenScript on real hardware):

```
tid is task_spawn of "kbd"
kbd_driver of tid
irq_claim of 1
6 * 7                      # typed THROUGH the .eigs driver
irq_release of 1
```

The break key (the machine cannot be bricked from its own REPL):

```
i is 0
loop while 1:
    i is i + 1
                           # blank line runs it — then press ctrl-c
6 * 7                      # prompt is back
```

The desktop (touchpad is PS/2 aux → IRQ12; drag, wheel, launcher):

```
import desktop             # ESC quits
```

## 4. Afterward

- Record verdicts, console mode, anomalies, and photos in
  docs/T3200-BOOT-LOG.md; push.
- If the stick proved the kernel: install the everyday path —
  `sudo cp build/EigenOS.elf /boot/` + the GRUB menuentry (recipe in
  CLAUDE.md). Iteration loop becomes edit → make → cp → reboot.
- Failure at any rung: the screen photo is the record. Text-mode
  verdicts identify the failing subsystem; a hang before the board
  means boot-stub/GRUB territory (try the GRUB menuentry path with a
  locally built ELF next).
