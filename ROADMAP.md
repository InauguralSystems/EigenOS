# EigenOS roadmap

## What this OS is

EigenOS is **niche but general**, the same shape as the language it exists
for. EigenScript's niche (observer semantics — free, total measurement of
every assignment) was never a limit on what it can do; the consumer repos
prove generality axis by axis (an emulator, a SAT solver, a compiler, a
transformer), each written in the observer idiom. The OS inherits that
structure:

- **General purpose defines the checklist**: storage, multitasking, time,
  devices, eventually networking. A computer you can actually use.
- **The niche defines how each item is answered**: in the observer-native
  idiom, not by imitating POSIX. Files-as-byte-streams and fork/exec are
  Unix's answers to Unix's questions; EigenOS answers the same needs with
  the primitives EigenScript already proved hosted — observed bindings,
  temporal queries, durable determinism.

The end state, in three layers (each useful if we stop there):

1. **The EigenScript machine** — boot → REPL → `define` → the machine does
   it. One language as shell, userland, and services; no impedance layer
   between using the computer and programming it (the Lisp-machine /
   Oberon lineage). Its finished form is **the desktop (MD)** — general
   purpose in the personal-computer sense: a screen with your things on
   it, several alive at once, all of it written in EigenScript.
2. **The self-observing computer** — the observer extended from program
   bindings to *machine* bindings: heap pressure, tick jitter, IRQ rates
   as observed values, so `converged of heap_pressure` is a kernel-policy
   primitive, not a monitoring query. Every other OS bolts measurement on
   afterward and pays for it; here it is the substrate.
3. **The replayable machine** — the trace tape as system journal:
   `prev of` / `state_at` against machine state, crash recovery as log
   replay. The composition of what liferaft (deterministic replay under
   chaos) and tidelog (crash-recoverable durable determinism) already
   proved hosted, applied to the whole computer.

And throughout: EigenOS is a **forcing function on the language**. Every
subsystem stresses EigenScript on an axis no hosted repo can reach — it has
already found the NaN-box low-canonical-malloc coupling and the
12.7 KiB-per-AST-level compiler stack frame, both invisible to every hosted
gate. When a milestone hits a language gap, the gap is surfaced and fixed
upstream, never worked around.

## Operating principles

- **Forcing-function ordering.** Build next whatever has the highest
  language-gap yield, not what Unix history says comes next.
- **No POSIX imitation.** Before adding a Unix-shaped interface, ask what
  the EigenScript-native answer is.
- **Small, serially verified milestones.** Every milestone lands with
  on-boot self-tests observable over serial (`make serial`), the way
  M0–MN did.
- **Prove outside, assert inside.** Invariants get a hosted proof
  (EigenScript CI: the embed REPL soak under a 64 KiB stack rlimit, the
  freestanding symbol gate, the mini-libc glibc differential) and a
  bare-metal assert (the boot-stack canary, on-boot self-tests). Hosted
  Linux is the only place sanitizers exist; use it for everything it can
  see before trusting the metal.
- **Target-hardware gates run in QEMU first.** `-cpu core2duo -m 3840
  -smp 2` approximates the T3200 (Penryn: SSSE3, no SSE4, no RDRAND) —
  the kernel must stay clean of post-Merom instructions, boot at 3.8 GiB,
  and boot from the ISO attached as a raw disk (the USB path; the ISO is
  isohybrid). All three verified 2026-07-02.

## Phase 1 — the substrate (DONE)

- [x] **M0** boot: GRUB/Multiboot2 → long mode → VGA text
- [x] **M1** IDT + CPU exceptions + COM1 serial (the headless logging seam)
- [x] **M2** physical memory manager over the Multiboot2 memory map
- [x] **M3** virtual memory: 4-level paging + higher-half direct map
- [x] **M4** hardware interrupts: 8259 PIC + PIT timer + PS/2 keyboard
- [x] **M5** the EigenScript runtime runs freestanding (mini-libc/libm +
      HAL glue; kernel heap; SSE; TLS)
- [x] **MN** boot into an interactive EigenScript REPL (VGA console +
      keyboard; state persists across lines). The `#UD` heisenbug that
      blocked it was a **boot-stack overflow** (the compiler cost
      ~12.7 KiB of C stack per AST level — fixed upstream, EigenScript
      PR #361); the boot stack is now 1 MiB with a canary strip at
      `stack_bottom` checked after every eval. See KNOWN_ISSUES.md for
      the full post-mortem.

## Phase 2 — the EigenScript machine

- [x] **M6 — a REPL you can actually type into (DONE 2026-07-02).**
      Full shifted-symbol keymap (`src/keyboard.c`, `kmap_shift`);
      multi-line input in `eigs_repl` (a line ending in `:` opens a
      block that accumulates; a blank line submits — so `define`, `for`,
      and observer loops are typed interactively); `set gfxpayload=text`
      in grub.cfg; `make usb USB=/dev/sdX` (guarded — refuses to guess a
      device). Verified end-to-end in QEMU via monitor `sendkey`:
      `a is 6*7`→42, string concat with `"`/`+`/`!`, a typed multi-line
      `define` (`sq of 12`→144), a typed observer loop (`converged`→1),
      list literals via brackets; T3200 sim gate re-run green.
- [ ] **M7 — boot the T3200.** The crossing from emulated to real.
      The target is a **Gateway M-7317U** (inventory: docs/T3200-INVENTORY.md;
      boot experiments: docs/T3200-BOOT-LOG.md): GM45/ICH9M, CPU flags
      match the sim gate exactly, ~3.94 GiB fragmented e820 (PMM verified
      hole-safe), Wi-Fi works, **no serial**, no FireWire/ExpressCard,
      flaky disk. **Boots via BIOS/CSM natively** (/sys/firmware/efi
      absent — the earlier "UEFI" conclusion was an unverified inference;
      an inactive ESP exists). **DRAM does NOT survive warm reset**
      (sentinel-tested), so the RAM crash-log ring is a dev-side/QEMU
      instrument only; on this hardware the post-mortem channels are the
      VGA screen + panic beep codes (2 = CPU exception, 3 = canary), and
      all boot/demo verdicts render to VGA (rows 17–19 + the board).
      Attempt plan (simplified by the CSM finding — no firmware changes
      needed, real VGA text is the native mode):
      1. **USB stick** (`make usb USB=/dev/sdX`, F10 boot menu at
         power-on, Setup = F2) → BIOS GRUB → VGA text → verdicts on
         screen → the REPL.
      2. Once proven: the **GRUB menuentry dual-boot** becomes the
         everyday no-media path — BIOS GRUB gives the same VGA text (the
         GOP-blindness caveat applied only to an EFI boot path).
      3. UEFI insurance already banked: the kernel boots green under
         OVMF+GRUB-EFI (matters for future hardware / if the boot mode
         ever changes; a framebuffer console would be needed for video
         in that mode).
      Success = the M5/MN self-tests green on screen and a hand-typed
      observer loop converging on 2008 silicon.
      Later upgrade (only when live debugging earns it): a gdb remote
      stub — but with no UART on this machine it would ride the M13 NIC,
      so it moves behind networking. USB-to-USB is a dead end (both
      machines are USB hosts; a bridge cable would need a full USB host
      stack in EigenOS).

- [x] **M7.5 — the machine has software (DONE 2026-07-03).** The
      upstream **source-provider seam** landed (EigenScript PR #362:
      `eigs_set_source_provider` — `import` consults the provider first
      in every profile; the only module source freestanding; M10's real
      filesystem later re-backs the same seam). EigenOS side: the **ROM
      bundle** — `tools/mkbundle.py` bakes the entire EigenScript stdlib
      (67 modules) + `demos/life.eigs` (~505 KiB of source) into the
      image as `.rodata` byte arrays; `hal.c` registers the bundle as
      the provider at eigs_open. Boot self-test: `import math` →
      `abs(-7)=7` verdict on VGA row 20. Verified in QEMU via sendkey:
      `import math`, `math.abs of -7` → 7, and **`import life` runs
      Conway's Game of Life on the console** — a glider crossing a
      toroidal grid, observer report on the population — real software
      on a machine with no filesystem. core2duo/3840 gate green.
      (Also fixed upstream: build.sh had drifted from the Makefile —
      missing eigs_embed.c — the known drift trap, caught by the matrix
      CI jobs.)

## Phase 3 — the self-observing computer

- [x] **M8 — machine vitals as observed bindings (DONE 2026-07-03).**
      Six live kernel counters exposed as builtins from hal.c
      (`heap_used`, `heap_mapped`, `mem_free_pages`, `mem_total_pages`,
      `ticks`, `tick_hz`) + a bundled `vitals` module wrapping them
      (`heap_pressure`, `mem_free_ratio`, `uptime_s`, `watch_heap`).
      The forcing-yield question answered itself cleanly: NO new
      observer semantics were needed — sample the vital into an ordinary
      binding in a loop and the binding's observer slot accumulates the
      trajectory; the vital IS just a value. Boot self-test (VGA row
      21): 14 samples of heap_pressure → `stable of hp` = 1 — the
      machine observes itself at every boot. REPL-verified via sendkey:
      the typed loop, `stable of hp` → 1, `report of hp` → equilibrium,
      `vitals.uptime_s of []` → seconds since boot. A leak now reads as
      `improving`/`diverging` on heap_pressure long before it is a
      crash. core2duo/3840 gate green.
- [x] **M9 — multitasking (DONE 2026-07-03).** The niche answer held:
      **a task IS an `EigsState`** — its own global env, module cache,
      observer thresholds — and a step is one eval on it. The upstream
      seam is `eigs_thread_switch` (EigenScript PR #363): parks the
      calling thread's attachment with NO teardown and activates another
      state's, all on the single CPU thread; the arena/freelists/VM live
      on the EigsThread and survive the switch. hal.c exposes
      `task_spawn/step/read/alive/kill` (a task program is a bundle
      module with a `step()` fn); only numbers cross states. The
      **scheduler is EigenScript** (`demos/sched.eigs` round-robins;
      policy is the observer): a task whose `progress` reads `stable` is
      stalled. Boot self-test (VGA row 22): spawn task_counter, step ×3,
      read progress = 3, kill. REPL-verified via sendkey: two tasks
      (task_counter, task_stuck) stepped in a loop — `stable of pa` = 0
      (healthy, advancing), `stable of pb` = 1 (stalled) — scheduling
      policy as an observer predicate, live on the console. Cost a real
      bug: the first cut matched attachments by `&eigs_current`, which
      miscompiles under the hand-rolled TLS → every switch re-attached →
      16 MiB arena leak per step → OOM; fixed to identify by state (PR
      #363). core2duo/3840 gate green.

- [x] **M9.5 — pixels and a pointer (DONE 2026-07-02).** The Multiboot2
      header requests 1024x768x32 (optional flag); the console became a
      cell GRID (vga.c) rendered by either backend — 0xB8000 text, or
      font-blit onto the LFB (fb.c, the ext_gfx `font5x7` extracted at
      build time by `tools/mkfont.py` — hosted windows and the metal
      console draw the same glyphs, 85x48 cells). The LFB sits above
      ramtop (outside the direct map), so early boot rows buffer in the
      grid and repaint once `map_page` can reach it; a kmalloc'd shadow
      buffer takes all gfx drawing and `gfx_present` copies to VRAM
      (VRAM is never read — one self-test probe excepted). ext_gfx's
      **second backend** in hal.c: gfx_open/close/clear/rect/line/
      point/circle/text/present/poll/ticks/delay + mouse_x/y/buttons,
      hosted-shaped event dicts (Tidepool's `ev.type`/`ev.key` loop
      idiom runs as-is); gfx_open suspends the console (grid still
      records), gfx_close/eval-error repaints it. PS/2 mouse on IRQ12
      (i8042 aux, AUXB-checked routing in both IRQ handlers, 3-byte
      packets). IRQ-path blitter is kernel-flags with rep-string asm —
      `objdump` audit: zero xmm, zero mini-libc mem* calls. Gates:
      boot self-test row 23 (script draws, kernel reads the pixel back
      from VRAM); screendump proof of console + `import draw` mouse
      painting + erase + console restore; core2duo/3840/smp2 green;
      USB dd-path green; **OVMF/EFI green — GOP framebuffer at a
      different address renders the same console, EFI-blindness fixed
      as predicted**; `-vga none` falls back to the text console with
      the gfx self-test a documented skip.
      **Input-dispatch groundwork for MD (DONE 2026-07-03):** the
      recorded debts are paid — E0 extended scancodes (arrows, home/
      end/delete/pgup/pgdn), modifier tracking (shift/ctrl/alt), a
      key-EVENT stream (down AND up, with modifiers) beside the char
      stream — gfx mode routes input to events so a quit key never
      replays into the REPL — and the IntelliMouse wheel unlock
      (sample-rate 200/100/80 knock; 4-byte packets; SDL-convention
      `wheel` events, psmouse-style sign). On top of it: **REPL line
      editing** — visible inverse-video cursor (console-cursor support
      in vga.c), left/right/home/end/delete, insert-at-cursor, 8-entry
      up/down history, ctrl-a/e/u/c — serial stays a faithful log (the
      line is written once, on submit). QEMU-verified end-to-end:
      recall-edit-eval (`1+2` → recalled → edited to `12` → 12),
      keydown/keyup pairs, wheel ±1 both directions, button events,
      draw.eigs regression. Remaining known subset: no gfx_rrect/
      gfx_fb, key events have no SDL scancode field.
- [x] **MD — the desktop (DONE 2026-07-03).** The general-purpose capstone: EigenOS as a
      personal computer in the full sense, and the niche answer to it —
      **windows are observed bindings**. A window system written in
      EigenScript over M9 tasks (window = task = `EigsState`) and M9.5
      pixels: launcher, the REPL as a first-class window, the M8 vitals
      monitor as the first widget. Every desktop ever built bolts on a
      task manager afterward to ask "what is the machine doing?" — here
      a hung app IS `diverging`, a settled one IS `converged`, for free.
      Forcing yield: real-time input-event dispatch across EigsStates,
      the gfx HAL under sustained load (Tidepool's frame-limiter lessons
      apply), and whatever a window manager written in an observer
      language turns out to need.
      **Phase 1 DONE (2026-07-03) — the windowed-drawing substrate +
      WM skeleton.** HAL seams: `task_view` (the WM assigns a window
      task a viewport; the task's gfx calls are translated into it and
      hard-clipped by the kernel — fb.c clip rect, not politeness — a
      task with no viewport draws nowhere); gfx builtins + `win_w/
      win_h/win_key` + vitals registered into every task state at
      spawn; `task_key` forwards key CODES to a per-task queue
      (numbers only cross states) and key event dicts carry a `code`
      field to make that a one-liner; `gfx_poll` in a task returns
      null (the WM owns input). `demos/desktop.eigs` is the WM in
      script: chrome + focus-follows-click + key dispatch to focus +
      per-frame `task_read` of each window's progress into a binding —
      **the stuck worker's title bar renders red "(stalled)" because
      `stable of` its progress says so**: the thesis, on screen.
      Screendump-verified (three windows, bounce animating, focus
      switch by click); key dispatch proven deterministically at the
      REPL (2× task_key + 1 step → count 21). Under TCG the desktop
      runs ~5-7 fps (cross-state evals dominate — the known M9 TCG
      cost; real silicon is the fix, not tuning).
      **Drag + z-order landed same day (WM-script only, zero C):**
      title-bar drag reassigns the viewport live (`task_view` per
      mousemove), focused window draws last and hit-tests first —
      screendump-proven with the counter dragged over bounce, then
      focus clicked back to bounce stacking above it. The full-redraw
      frame makes overlap correct without damage tracking (defer
      dirty-rects until a perf need is measured).
      **The REPL as a window (DONE 2026-07-03).** The seam:
      `task_eval [id, "src"]` — the WM sends a typed line to a window
      task's state (idle between steps, so it is the same outside-in
      eval task_step does — no VM nesting) and the result comes back
      formatted as a BY-VALUE string; the cross-state rule extends
      from numbers to copied strings, never pointers. Plus
      hosted-parity `gfx_clip` so the WM can render the transcript
      bounded to the window rect. The WM owns the input line (it owns
      key events anyway); `demos/win_repl.eigs` is just the home
      state + heartbeat. Typed `gfx_*` INSIDE the window draws into
      that window's viewport (g_active_task is set during the eval).
      QEMU-proven: `x is 5` → 5, `x * 8` → 40 in the window, and after
      quitting, `x` at the boot console is UNDEFINED — a REPL window
      is an isolated machine state you can look at. ESC now quits the
      desktop (q is typeable text).
      **The vitals widget (DONE 2026-07-03)** — the M8 monitor as the
      first widget, zero new kernel surface (task states already had
      the vitals builtins): `win_vitals.eigs` samples heap_pressure /
      mem_free_ratio / uptime into its own bindings each step, draws
      bars, and renders the machine's own verdict — "heap: steady"
      via `stable of`, "heap: growing" via `diverging of` — the
      self-observing computer in a window, screendump-proven with all
      five windows composited.
      **The list-driven WM (DONE 2026-07-03)** — the per-window
      branching is gone before it met a sixth window: each window is a
      record (`{id,x,y,w,h,title,kind,slot}`) in a `wins` list ordered
      back-to-front (focused = last), and focus, hit-test, drag, key
      routing, draw, and teardown all iterate it — adding a window is
      one `spawn_win` call. Hit-testing scans front-to-back, which
      also fixed a latent hit-vs-z mismatch in the branched code (the
      old fallback probed windows in fixed spawn order, so an occluded
      window could steal a click). The one thing the language keeps
      NAMED: observer slots — a trajectory accumulates on a binding,
      and a list element has no binding identity, so progress samples
      land in fixed scalars `p0..p7` behind a slot dispatch;
      `spawn_win` throws on a 9th window rather than silently dropping
      observation. Proven twice: a hosted twin harness (stubs the
      task/gfx seams in pure EigenScript, scripts an event tape, 26
      assertions on z-order/drag/keys/repl/teardown/observer verdicts)
      and the QEMU drive (click-to-focus, drag with live viewport,
      the occluded-title click going to the topmost window, `6 * 7` →
      `42` in the repl window, keys +10 to the counter, stalled title
      red, clean ESC teardown).
      **Resize (DONE 2026-07-03)** — a bottom-right grip on every
      window (corner dots drawn after `task_step` so the task's own
      painting can't cover them); a grip press starts a resize instead
      of a drag, the grabbed corner tracks the pointer through the
      same `task_view` seam, and 120x60 minimums keep the title and
      the grip reachable. Zero kernel changes — `task_view` never
      assumed a fixed size, tasks already adapt through `win_w`/
      `win_h`, and the kernel clip does the rest (the bounce ball
      re-clamps into a shrunk window on its next step). Twin-harness
      assertions cover the grow arithmetic (via a viewport history
      log), both minimum clamps, and that a grip press never moves
      the window; QEMU-proven growing bounce to 440x300 over its
      neighbors, then shrinking it far below minimum onto the 120x60
      clamp.
      **Launcher + close (DONE 2026-07-03)** — the window LIFECYCLE
      pair, WM script only: a top launcher bar (one chip per app from
      a data table, cascade placement, launched window arrives
      focused) and a title-bar x that kills the task and returns its
      observer slot. The slot budget became a free pool; spawn_win
      refuses past it (bar shows the free count, red at zero — with a
      launcher, "full" is a reachable user state, not a programming
      error, so the throw became a visible refusal). The kernel table
      needed nothing: MAX_TASKS was already 8 and task_kill already
      frees ids for reuse — the twin harness stub now models that id
      reuse kernel-faithfully. repl line/transcript moved into the
      window record, so every launched repl window is independent.
      **The launcher work also caught a live observer finding**: raw
      progress counters false-read `stable` in the ~20..80 magnitude
      band and go entropy-dark past ~80 (the #294 flat-entropy blind
      spot — entropy_of_num decays with magnitude; report_value's
      relative step decays too). The WM now observes a bounded cyclic
      signature, `1.5 + (progress % 8)`: a wedged task is a nonzero
      constant (stable at any magnitude, forever), an advancing one
      cycles (never stable) — instrument choice, not a language
      workaround; caught by the twin harness at 30 samples, invisible
      in every previous long-warm-up screendump. Proven: harness
      (46 asserts: launch cascade, budget exhaustion, refusal BEFORE
      task_spawn, close frees slot+id, relaunch reuses both, reused
      slot's verdict recovers within one window) + QEMU (3 launches,
      "0 free" red, refused 4th, x-close, relaunch into the freed
      slot; a freshly launched stuck worker reads red within its
      first window).
      **Wheel to focus (DONE 2026-07-03)** — scroll routes to the
      focused window exactly like keys, through a new cross-state
      pair: `task_wheel [id, dy]` (WM-side) ACCUMULATES deltas —
      scroll is additive, so an accumulator, not a queue (clamped
      ±32767 so a window that never reads can't overflow) — and
      `win_wheel of []` (task-side) reads-and-clears the sum.
      win_counter displays its running wheel sum, so routing is
      visible and exact. Twin harness: wheel events land on whichever
      window held focus at that moment. QEMU: three notches → the
      focused counter reads "wheel: 3" (sign matches the SDL y=+1-up
      convention end to end); focus moved to bounce, two more notches,
      the counter's sum stays 3.
      **Block input in REPL windows (DONE 2026-07-03)** — the boot
      console's block contract, per window: a line ending in `:`
      opens a block (the prompt turns `....>`), lines accumulate in
      the window record's `rblock`, and a blank line submits the
      whole block as ONE `task_eval` on the window's own state. Pure
      WM script — the kernel seam already took arbitrary source.
      Twin harness: the block joins with newlines, submits once,
      echoes `eigs>`/`....>` correctly, clears its state. QEMU: a
      two-line `define d(n) as: / return n * 2` typed through the
      `....>` prompt into the repl window, then `d of 21` → `42` on
      that window's isolated state. MD IS COMPLETE — the desktop
      milestone's full arc (viewported window tasks, drag, z-order,
      resize, launcher + close over the slot pool, key + wheel
      dispatch to focus, independent block-capable REPL windows,
      observer-verdict titles) landed as WM script over five small
      kernel seams.

## Phase 4 — the durable, replayable machine

- [x] **M10 — storage: tidelog as the filesystem (DONE 2026-07-03).** The general need:
      files that survive a reboot. The niche answer: a log-structured,
      crash-recoverable store with time-travel reads (tidelog proved the
      whole design hosted — CBOR codec, every-offset crash sweep,
      compaction with atomic swap) sitting on a block driver (ATA PIO
      first; the T3200 has a plain SATA spinner). Un-carve the
      freestanding profile's filesystem builtins (`read_text`,
      `write_bytes`, `load_file`/`import`, the package tree) onto it.
      Forcing yield: the durability primitives (#248–250) re-proven with
      no OS underneath; what `import` means when the "filesystem" is a
      replayable log.
      **Phase 1 — the block substrate (DONE 2026-07-03).** `src/ata.c`:
      polled ATA PIO, no IRQ path at all (nIEN held, IRQ14/15 stay
      masked), one sector per command, every wait iteration-bounded so
      a dead drive FAILS the verdict instead of hanging the boot.
      LBA48 when the identify advertises it (the T3200's 320GB spinner
      needs it past 128GiB), LBA28 fallback; ATAPI devices (the boot
      CD) recognized by signature and skipped. EigenScript-visible:
      `blk_present/sectors/model/read/write/unlock/flush` — sectors
      travel as lists of 512 byte numbers for now (the embed API has
      no buffer surface; adding one upstream is phase 2's first move,
      since tidelog's data path is buffer-based). SAFETY BAKED IN:
      the boot self-test is READ-ONLY (identify + LBA 0), and
      `blk_write` refuses until an explicit `blk_unlock` that boot —
      on the T3200 the primary disk carries the Linux install; the
      store's mount code will unlock only after verifying the tidelog
      magic, and nothing ever auto-formats. Proven: `make serial-disk`
      (64MiB scratch image) → write a 512-byte pattern to sector 100,
      flush, REBOOT THE GUEST, read it back intact before any unlock
      — bytes survive a reboot for the first time; the fresh boot's
      locked `blk_write` refused loudly; no-disk boot reports a clean
      absent verdict; core2duo/3840/smp2 gate green, zero post-Merom
      instructions. **Buffer seam (DONE 2026-07-03)**: upstream PR
      #364 (merged) added the embed buffer API
      (`eigs_value_new_buffer/_len/_get/_set` + `EIGS_TYPE_BUFFER`);
      blk_read/blk_write now speak real buffers — NUL-safe and the
      exact shape tidelog's store I/O uses. QEMU-proven: list-era
      sector reads back through buf_get; a buffer-written sector
      round-trips exactly and survives a guest reboot. Tidelog's gap
      ledger (FINDINGS.md) read against the freestanding carve: the
      CBOR codec's primitives (#248: str_from_bytes, f64_to/from_bytes
      + the buffer family) are UNCARVED and should run on EigenOS
      as-is; the store's whole I/O seam (read_bytes_buf, append-mode
      write_bytes from #249, rename + remove_file from #250) is
      EXACTLY the carved set. The one gap that gets harder on bare
      metal: #250's atomic rename was borrowed from rename(2) — the
      byte-region layer must EARN crash-atomic swap itself
      (double-buffered region table + generation counter + ordered
      flushes), and tidelog's every-offset crash sweep re-runs hosted
      against a simulated block device as the oracle.
      **The byte-region layer (DONE 2026-07-03)** — `demos/regionfs.eigs`,
      pure EigenScript over blk_*: tidelog's exact file-shaped seam
      (`read_bytes_buf`, create-on-write/append `write_bytes` taking
      list or buffer, `rename` with POSIX replace-over-existing,
      `remove_file`, `file_exists`) on a double-buffered-superblock
      layout — magic + 6-byte generation + inline region table +
      Adler-32 (all coding in F-CBOR-1 exact arithmetic), fixed-size
      slots behind it. THE COMMIT DISCIPLINE EARNS rename(2): data
      sectors → flush → new table at generation+1 into the INACTIVE
      superblock copy → flush; mount takes the valid copy with the
      higher generation, so every op lands atomically on old-or-new
      state (assumed model: 512-byte sector writes are atomic — the
      same assumption hosted tidelog inherits from the page cache);
      a torn superblock write fails its checksum and the sibling
      copy wins. Safety composes: mount verifies the magic BEFORE
      blk_unlock (the T3200's Linux disk reads as an MBR → mount 0 →
      writes never unlock), and format refuses unless the operator
      unlocked by hand. PROVEN: hosted twin
      (tools/regionfs_twin_hosted.eigs, 74 checks) = functional suite
      + THE CRASH SWEEP — every sector-write prefix of six ops
      (format, multi-sector create, RMW append, create, rename-over-
      existing, remove) mounted from a crashed-disk snapshot and
      required to equal exactly the pre- or post-op committed state;
      QEMU: mount refused the unformatted scratch disk, format, a
      600-byte write + 100-byte RMW append read back exact (70057
      signature), GUEST REBOOT, mount-by-magic auto-unlock, bytes
      intact, rename verified under the new name.
      **The store, mounted (DONE 2026-07-03)** — `import tidefs` =
      tidelog's `store.eigs` + `cbor.eigs`, BYTE-UNMODIFIED, on
      regionfs. The composition is a build-time concatenation
      (regionfs first, so its defines provide the carved fs-builtin
      names in the scope the store resolves them from — the same
      load_file composition tidelog.eigs does hosted, done in the
      Makefile because load_file is carved). ZERO findings: the four
      primitives the tidelog port drove upstream (#248 str_from_bytes
      + f64 codecs, #249 append write_bytes, #250 rename/remove_file)
      were exactly sufficient — nothing else was missing, nothing was
      worked around. Hosted twin (tools/tidefs_twin_hosted.eigs, 21
      checks): ops + recovery-equivalence reopen, temporal reads,
      tidelog's own crash model re-run on this substrate (an
      every-byte-offset truncation sweep of the log, recovery landing
      exactly on the committed-record-prefix state at ALL offsets),
      compaction through the earned rename (temp gone, log smaller,
      byte-deterministic). QEMU: puts/overwrite at the REPL, GUEST
      REBOOT, store_open replayed the log to version 3 with values
      intact, and `store_get_at` read the PRE-OVERWRITE value from
      behind the reboot — time travel over durable machine state on
      bare metal; store_compact live on the metal too. Constraint
      noted: store paths ≤ 12 chars (the ".compact" temp must fit
      regionfs's 20-char names).
      **The un-carve (DONE 2026-07-03)** — the carved fs builtins
      (`read_text`, `write_text`, `read_bytes_buf`, `write_bytes`,
      `file_exists`, `rename`, `remove_file`, + EigenOS's `fs_mount`/
      `fs_format`) are provided by hal.c through the embed API — the
      vitals/tasks/gfx pattern, no upstream change. Each delegates to
      the FS DAEMON STATE: a dedicated EigsState running the
      byte-unmodified regionfs module, reached by task_eval's proven
      park-switch-eval-return shape (the fsd is always idle between
      calls — no VM nesting), bytes crossing BY COPY through a C
      staging buffer. This also fixed the layering: `tidefs` is now
      cbor+store only, its bare fs calls resolving to the delegates —
      ONE mount authority (the fsd), stores usable from any state,
      windows included (register_task_side gets the fs surface).
      Mount policy sharpened: mount-by-magic IS the authorization, so
      the boot self-test probes it — "[m10] fs: regionfs mounted, N
      file(s)" on a store-bearing disk, "no store (mount refused;
      writes stay locked)" on a foreign one. QEMU-proven: bare
      `write_text`/`read_text`/`file_exists` at the REPL; the
      relayered store opened the PREVIOUS session's compacted log
      through the new plumbing (store.eigs → C delegate → fsd →
      regionfs → ATA); a put, a reboot, and both the text file and
      the new key recovered, with the boot probe counting 2 files.
      DISPOSITION on `load_file`/`import`/the package tree: they stay
      ROM-bundle — on EigenOS the image IS the software distribution
      and the store serves DATA; a store-consulting import is a
      deliberate non-goal until M12's .eigs drivers give a reason to
      load code from disk. With that disposition, M10's arc is
      complete.
- [x] **M11 — the system journal (DONE 2026-07-03).** The trace tape as machine journal:
      `prev of` and `state_at` against system state, boot-to-now history
      as a first-class object, crash recovery as replay rather than fsck
      archaeology. Forcing yield: the trace-tape file sinks are carved
      out of the freestanding profile today; they come back targeting
      M10's store, and determinism gets pressure-tested with no host
      clock underneath (the PIT is the only time source).
      **Phase 1 — the tape reaches the store (DONE 2026-07-03).** The
      named forcing yield landed: upstream PR #365 (merged) exposed
      the trace tape through the embed API — `eigs_set_trace_sink`
      streams every record as bytes (no filesystem), `eigs_set_replay_tape`
      installs a whole tape as the in-memory replay source, and
      `eigs_replay_take`/`eigs_trace_record_nondet` let HOST builtins
      participate with the runtime's own take/record contract. The
      seam surfaced two real fixes: memory tapes never served (replay
      gated on the FILE alone) and the strict-mismatch `_exit` became
      reachable freestanding (the symbol gate caught it; `abort()` is
      the HAL root there). EigenOS side: the machine vitals (`ticks`,
      `heap_used`, `heap_mapped`, `mem_free_pages`, `gfx_ticks`,
      `mouse_x/y/buttons`) are now tape-participating NONDET sources —
      machine readings are inputs, recorded and replayable — and
      `journal_on/off/replay/stop` wire the tape to the M10 store
      ("journal" file) through the fsd, with the sink append-only into
      a kheap buffer (never re-enters the runtime mid-eval; flushed on
      journal_off). QEMU-PROVEN, the replayable machine's first breath:
      a session read ticks=2133 and heap_used=42418600, journal_off
      persisted 68 tape bytes, the GUEST REBOOTED, journal_replay
      loaded the tape, and the same session re-derived EXACTLY
      2133/42418600/21333 while the machine's live clock stood at 4228
      — recovery as replay across a power cycle; journal_stop resumed
      live readings. Contract (tidelog's, one level up): deterministic
      given the durable input.
      **Phase 2 — the session journal (DONE 2026-07-03).** The REPL's
      input lines join the tape as `I <escaped-source>` records,
      interleaved with the runtime's L/A/N records in the same journal
      (the runtime's replay reader skips non-N lines by contract, so
      no upstream change was needed — the tape format is extensible
      from the embedder side). A multi-line block is one I record.
      `journal_replay of []` now replays the WHOLE SESSION in one
      command: it loads the journal, installs the nondet tape, parses
      the I records, and the REPL loop drives them BETWEEN evals (the
      only legal spot — an eval inside a builtin would nest the VM),
      echoing each unit with reconstructed prompts; replay mode then
      ends so live typing never consumes stale records. journal_*
      meta-lines are never recorded (a replayed session must not
      rewrite the journal it replays from). QEMU-proven: record
      `t is ticks of []` (3082) + a multi-line define + `d of 3`
      (9246), reboot, one command — the session re-drives itself on
      screen with the RECORDED readings while the live clock reads
      3027, and afterward the session's BINDINGS exist live (`d of 3`
      → 9246 with t bound to the recorded 3082): replay reconstructs
      state, not just output — crash recovery as replay, in
      miniature.
      **Phase 3 — the auto-journal ring (DONE 2026-07-03).** The
      machine is its own flight recorder: when a store mounted at
      boot, the session journals itself — no operator bracket. The
      ring lives in RAM (64 KiB cap; on overflow it slides to the
      newest half, cutting at an I-record boundary so the surviving
      tail stays replay-coherent — a burst that evicts all I records
      drives nothing, harmlessly: bounded evidence, newest wins), and
      the REPL loop truncate-writes the whole ring to the store file
      "autolog" between evals — the file is a snapshot of the ring,
      no rotation machinery, crash loses only the since-last-eval
      tail. Every boot ARCHIVES the previous ring as "lastboot"
      before the first flush can touch it; after an unexpected death:
      `journal_replay of "lastboot"`. Two real bugs found and fixed
      during verification: (1) the recorder recorded its own flushes
      — the trace sink is process-global, so the fs daemon's per-byte
      write loops poured thousands of A records into the ring being
      flushed, each flush slower than the last; all fsd evals now run
      behind a g_fsd_busy gate (machinery is not session activity);
      (2) journal_* meta evals' L records dirtied the ring so a
      replay-only session clobbered "autolog" — meta evals now
      suspend the sink. The manual journal_on/off bracket still works
      (auto pauses, resumes after). QEMU-proven: a session (x from
      ticks, two derived bindings) with NOTHING manually saved, the
      machine KILLED mid-session, rebooted — one command re-drove all
      three lines with the recorded readings and the dead session's
      bindings were live again; byte-dumps of the ring at every stage
      verified the pipeline (I/L/A/N records exactly as designed).
      **Phase 4 — machine history, time-keyed (DONE 2026-07-03).**
      `state_at of L` is line-keyed (a program's assignment timeline);
      machine time is the other axis — tidelog's F-TEMPORAL-1 drew
      exactly this boundary, so the machine-native answer is
      TIME-keyed history: a sampler ring (4096 samples, ~68 min at
      1 Hz) records the vitals from the REPL's idle loop and the gfx
      frame path (cooperative machine, cooperative sampler), boot to
      now, and three queries expose it — `vital_at of [name, t]` (the
      state_at shape: the machine's reading at a past moment; null
      before history), `vital_series of [name, n]` (a buffer
      trajectory), `history_span of []`. All three are
      tape-participating nondet, so journaled history queries replay
      exactly (series buffers ride the N records' b[...] form).
      QEMU-proven: 99 samples accumulated; a 16 MB allocation made
      between two remembered instants, then
      `(vital_at of ["heap_used", t1]) - (vital_at of ["heap_used", t0])`
      answered 1.62881e+07 FROM THE RECORDED TIMELINE; and the
      composition line — a `vital_series` trajectory poured through
      an observed binding gave `report_value of hh` → "moving": the
      observer passing verdicts on the machine's own past.
      **Phase 5 — the chaos sweep (DONE 2026-07-03, closes M11).**
      liferaft's discipline against the REAL pipeline:
      `tools/journal_chaos.py` runs seeded cycles in QEMU — each
      drives a fixed 8-unit session (machine readings, dependent
      arithmetic, a multi-line define, history queries, an fs write),
      KILLS the machine at a seed-derived moment (after unit k, with
      seed-jittered delay; every third seed dies mid-typing), reboots,
      replays "lastboot", and applies the TRANSCRIPT ORACLE: the
      replayed block must equal, line for line including the recorded
      machine readings, the prefix of the recorded transcript covering
      exactly the units that completed before death. The crash model
      is reduced by construction first — regionfs's double-buffered
      commit means a recovered tape is a COMPLETE earlier ring
      snapshot, old-or-new, never torn bytes — so the adversary's
      remaining axis is timing, which the sweep covers at every
      session position. 8/8 seeds clean. One finding, in the
      HARNESS's oracle, not the pipeline: an input-less session never
      writes the ring, so replay after a silent crash yields the last
      NON-EMPTY session — the recorder never overwrites evidence with
      silence; documented and asserted. With this, M11's arc is
      complete: the machine records itself, survives its own death,
      and answers questions about its past.

## Phase 5 — outward

- [ ] **M12 — devices in EigenScript.** Drivers as `.eigs` programs:
      port I/O, MMIO/volatile semantics, interrupt-driven callbacks from
      the language. Proof: reimplement the PS/2 keyboard driver in
      EigenScript and swap it in. The newest axis the portfolio has never
      touched — likely the richest untapped gap-finder (bytes, volatile,
      FFI, IRQ-safe callbacks).
    - [x] **Phase 1 — raw port I/O (DONE 2026-07-03).** `in{b,w,l}` /
          `out{b,w,l}` builtins in hal.c: mechanism only, out-of-range
          ports/values refused (never truncated), and the ATA register
          blocks closed to `out*` until `blk_unlock` — raw ports must
          not bypass M10's disk-write lock. `demos/pci.eigs` is the
          first script to touch hardware registers: config mechanism #1
          in pure EigenScript (`cfg_read`/`vendor`/`class_of`/`scan`);
          at the REPL it enumerated the machine — host bridge, PIIX3,
          VGA, and the e1000 as class 0002, the M13 target already
          visible from script. Boot self-test: `outl of [0xCF8,
          0x80000000]` + `inl of 0xCFC` reads vendor 0x8086 (i440FX/Q35
          and the T3200's GM45 all have an Intel host bridge at 0:0.0).
          FORCING YIELD (the milestone paid out on day one): hex
          literals only parsed via glibc strtod's C99 hex-float
          acceptance — hosted worked, freestanding lexed `0xFF` as `0`
          + identifier `xFF`. Fixed at the root (EigenScript #378): hex
          integers are lexed explicitly on every profile, hex-FLOAT
          forms (`0x1p4`) are loud parse errors, suite [50b] pins it.
          Next: the IRQ seam (`irq_claim`/`irq_read` — raw bytes divert
          to a per-IRQ ring the language drains; IRQ context stays pure
          C per the SSE invariant), then kbd.eigs swapped in as the
          proof.
    - [x] **Phase 2 — the IRQ-claim seam (DONE 2026-07-03).**
          `irq_claim/irq_read/irq_release/irq_dropped` over per-IRQ
          rings (`src/irqhook.c`, kernel-flags): a claimed IRQ's raw
          bytes divert BEFORE the C driver's translation (IRQ1 = raw
          scancodes, E0 prefixes and all; IRQ12 = raw aux bytes, both
          divert points 8042-aware), and the script drains the ring
          cooperatively — a driver's "interrupt callback" is the drain,
          so IRQ context never touches the runtime (the SSE invariant
          holds; XMM audit clean on irqhook/keyboard/mouse). irq_read /
          irq_dropped take the M11 tape like vitals. Input cannot die
          from script: releasing a claim falls back to the C driver,
          eval errors force-release all claims, and the REPL's key-wait
          is the claimant of last resort (a bare `irq_claim of 1` at
          the prompt heals itself, loudly — proven under sendkey).
          Self-test + demo (`demos/irqecho.eigs`): the 8042 output-
          buffer loopback (command 0xD2) — the script injects a
          scancode through the controller and receives it back through
          its own claim ring; `irqecho.echo of 0x2A` reads 42 at the
          REPL. Next: kbd.eigs — the set-1 state machine in EigenScript
          feeding translated input back, stepped by the REPL wait
          loop's driver pump.
- [ ] **M13 — networking (later).** A NIC driver + the HTTP extension
      freestanding. Deliberately last: biggest surface, lowest
      forcing-function novelty (the HTTP stack is already hardened
      hosted).

## Non-goals

- POSIX compatibility, or any compatibility layer.
- Multi-user, security domains, or ring-3 isolation before a milestone
  actually needs them (the kernel/task boundary is the runtime itself
  for now).
- Adoption. EigenOS's consumers are its author and the language it
  sharpens.
