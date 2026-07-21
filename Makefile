# EigenOS build. gcc/ld build the ELF64 kernel; GRUB (Multiboot2) boots it.
#   make            build build/EigenOS.elf
#   make verify     assert it's a valid Multiboot2 kernel
#   make iso        build a bootable ISO (needs grub-mkrescue + xorriso)
#   make run        boot the ISO in QEMU
#   make clean
CC      := gcc
CFLAGS  := -ffreestanding -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse \
           -mno-sse2 -Wall -Wextra -Wno-unused-parameter -O2 -c
LDFLAGS := -ffreestanding -nostdlib -no-pie -Wl,-T,linker.ld -Wl,-z,noexecstack \
           -Wl,--build-id=none
QEMU    := qemu-system-x86_64
# All QEMU runs get >= 1 GiB so the crash-log ring at 0x3FFF0000
# (crashlog.h) is RAM-backed; the default -m 128M would leave it
# outside RAM and silently dead in every dev run.
QFLAGS  := -m 1024

# --- EigenScript runtime, built freestanding into the kernel image (M5) ---
# The sibling checkout; the runtime is compiled WITH SSE (it is all doubles)
# and the freestanding profile, TLS forced to local-exec (statically linked,
# so %fs:tpoff is a link-time constant — no __tls_get_addr).
EIGS      := ../EigenScript
RT_CFLAGS := -ffreestanding -nostdlib -m64 -mno-red-zone -msse -msse2 \
             -fno-stack-protector -U_FORTIFY_SOURCE -ftls-model=local-exec \
             -DEIGENSCRIPT_FREESTANDING=1 -DEIGENSCRIPT_EXT_HTTP=0 \
             -DEIGENSCRIPT_EXT_MODEL=0 -DEIGENSCRIPT_EXT_DB=0 \
             -O2 -I$(EIGS)/src -c
MINI_CFLAGS := -fno-builtin -ffp-contract=off -fno-math-errno \
               -DEIGS_MINI_STANDARD_NAMES=1

EIGS_SRCS := eigenscript lexer parser builtins builtins_tensor hash arena \
             state strbuf ext_store fmt lint chunk compiler vm jit trace eigs_embed
EIGS_FS   := mini_libc mini_libm mini_fmt mini_strtod
EIGS_OBJS := $(addprefix build/eigs_,$(addsuffix .o,$(EIGS_SRCS))) \
             $(addprefix build/eigsfs_,$(addsuffix .o,$(EIGS_FS))) \
             build/hal.o

OBJS   := build/boot.o build/kernel.o build/pmm.o build/serial.o \
          build/idt.o build/isr.o build/paging.o build/pic.o \
          build/timer.o build/keyboard.o build/kheap.o build/vga.o \
          build/fb.o build/mouse.o build/crashlog.o build/ata.o \
          build/irqhook.o build/e1000.o build/bundle.o

# The ROM bundle (M7.5): the EigenScript stdlib + demos/ baked into the
# image, served via the eigs_set_source_provider seam — `import` on bare
# metal with no filesystem. Regenerates when any source or the tool moves.
#
# M10: `import tidefs` = tidelog's store, byte-unmodified (a build-time
# concatenation of cbor + store — the load_file composition tidelog.eigs
# does hosted; load_file is carved freestanding). Its bare fs calls
# (write_bytes/read_bytes_buf/rename/file_exists) resolve to the
# UN-CARVED builtins hal.c registers, which delegate to the fs daemon
# state running regionfs — one mount authority, stores on any state.
# Couples to the sibling ../tidelog checkout, like ../EigenScript.
TIDELOG := ../tidelog
build/gen/tidefs.eigs: $(TIDELOG)/src/cbor.eigs $(TIDELOG)/src/store.eigs | build
	mkdir -p build/gen
	{ cat $(TIDELOG)/src/cbor.eigs; echo; cat $(TIDELOG)/src/store.eigs; } > $@

# The solver window: `import win_sat` = EigenScript's int_vector +
# EigenMiniSat's CDCL solver (both byte-unmodified — solver.eigs is
# composition-free since EigenMiniSat #56) + the window body
# (demos/satwin.eigs, filtered out of the wildcard so the partial file
# is not a broken standalone module). Same concatenation pattern as
# tidefs. Couples to the sibling ../EigenMiniSat checkout.
# Twin gate: tools/win_sat_twin_hosted.eigs.
SAT := ../EigenMiniSat
build/gen/win_sat.eigs: $(EIGS)/lib/int_vector.eigs $(SAT)/lib/solver.eigs demos/satwin.eigs | build
	mkdir -p build/gen
	{ cat $(EIGS)/lib/int_vector.eigs; echo; cat $(SAT)/lib/solver.eigs; echo; cat demos/satwin.eigs; } > $@

# The editor window: `import win_edit` = eigen-edit's buffer core
# (../eigen-edit/edit.eigs, byte-unmodified) + the OS window body
# (demos/editwin.eigs, filtered out of the wildcard). EigenOS is a
# CONSUMER of eigen-edit, not its owner — the same edit.eigs runs
# standalone on the desktop and in the WASM playground. Same
# concatenation pattern as win_sat. Couples to the sibling
# ../eigen-edit checkout.
EDIT := ../eigen-edit
build/gen/win_edit.eigs: $(EDIT)/edit.eigs demos/editwin.eigs | build
	mkdir -p build/gen
	{ cat $(EDIT)/edit.eigs; echo; cat demos/editwin.eigs; } > $@

# The spreadsheet window: `import win_sheet` = eigen-sheet's recalc engine
# (../eigen-sheet/sheet.eigs, byte-unmodified) + the OS window body
# (demos/sheetwin.eigs, filtered out of the wildcard). EigenOS is a
# CONSUMER of eigen-sheet, not its owner — the same sheet.eigs runs
# standalone on the desktop and in the WASM playground. Same
# concatenation pattern as win_edit. Couples to the sibling
# ../eigen-sheet checkout.
# Twin gate: tools/win_sheet_twin_hosted.eigs.
SHEET := ../eigen-sheet
build/gen/win_sheet.eigs: $(SHEET)/sheet.eigs demos/sheetwin.eigs | build
	mkdir -p build/gen
	{ cat $(SHEET)/sheet.eigs; echo; cat demos/sheetwin.eigs; } > $@

BUNDLE_SRCS := $(wildcard $(EIGS)/lib/*.eigs) $(filter-out demos/satwin.eigs demos/editwin.eigs demos/sheetwin.eigs,$(wildcard demos/*.eigs)) build/gen/tidefs.eigs build/gen/win_sat.eigs build/gen/win_edit.eigs build/gen/win_sheet.eigs
KERNEL := build/EigenOS.elf
ISO    := build/EigenOS.iso

all: $(KERNEL)

build:
	mkdir -p build

build/boot.o: src/boot.S | build
	$(CC) -m64 -c $< -o $@

build/kernel.o: src/kernel.c src/pmm.h src/multiboot2.h src/serial.h src/idt.h src/paging.h src/pic.h src/timer.h src/keyboard.h src/kheap.h src/stackguard.h src/crashlog.h src/vga.h src/fb.h src/mouse.h src/ata.h src/e1000.h | build
	$(CC) $(CFLAGS) $< -o $@

build/pmm.o: src/pmm.c src/pmm.h src/multiboot2.h src/crashlog.h | build
	$(CC) $(CFLAGS) $< -o $@

build/serial.o: src/serial.c src/serial.h src/io.h src/crashlog.h | build
	$(CC) $(CFLAGS) $< -o $@

build/idt.o: src/idt.c src/idt.h src/serial.h src/pic.h src/timer.h src/keyboard.h src/mouse.h src/vga.h | build
	$(CC) $(CFLAGS) $< -o $@

build/isr.o: src/isr.S | build
	$(CC) -m64 -c $< -o $@

build/paging.o: src/paging.c src/paging.h src/pmm.h | build
	$(CC) $(CFLAGS) $< -o $@

build/pic.o: src/pic.c src/pic.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/timer.o: src/timer.c src/timer.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/keyboard.o: src/keyboard.c src/keyboard.h src/mouse.h src/irqhook.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/mouse.o: src/mouse.c src/mouse.h src/irqhook.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/irqhook.o: src/irqhook.c src/irqhook.h | build
	$(CC) $(CFLAGS) $< -o $@

build/ata.o: src/ata.c src/ata.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/e1000.o: src/e1000.c src/e1000.h src/io.h src/paging.h src/pmm.h src/timer.h | build
	$(CC) $(CFLAGS) $< -o $@

# The fb console's font is EigenScript's own gfx_text font, extracted
# from ext_gfx.c at build time (single source of truth, like the ROM
# bundle) — hosted windows and the bare-metal console draw the same
# glyphs.
build/font5x7.h: tools/mkfont.py $(EIGS)/src/ext_gfx.c | build
	python3 tools/mkfont.py $(EIGS)/src/ext_gfx.c $@

build/fb.o: src/fb.c src/fb.h src/paging.h src/kheap.h build/font5x7.h | build
	$(CC) $(CFLAGS) -Ibuild $< -o $@

build/kheap.o: src/kheap.c src/kheap.h src/pmm.h src/paging.h | build
	$(CC) $(CFLAGS) $< -o $@

build/vga.o: src/vga.c src/vga.h src/fb.h | build
	$(CC) $(CFLAGS) $< -o $@

build/crashlog.o: src/crashlog.c src/crashlog.h | build
	$(CC) $(CFLAGS) $< -o $@

build/bundle.c: tools/mkbundle.py $(BUNDLE_SRCS) | build
	python3 tools/mkbundle.py $@ $(BUNDLE_SRCS)

build/bundle.o: build/bundle.c
	$(CC) $(CFLAGS) $< -o $@

# EigenScript runtime objects (freestanding + SSE).
build/eigs_%.o: $(EIGS)/src/%.c | build
	$(CC) $(RT_CFLAGS) $< -o $@

build/eigsfs_%.o: $(EIGS)/src/freestanding/%.c | build
	$(CC) $(RT_CFLAGS) $(MINI_CFLAGS) $< -o $@

# The HAL glue: runtime cflags (SSE) so variadic FP + the double return
# from eigs_value_as_num are handled; -Isrc for the kernel headers.
build/hal.o: src/hal.c src/kheap.h src/serial.h src/timer.h src/keyboard.h src/vga.h src/fb.h src/mouse.h src/ata.h src/irqhook.h src/io.h src/stackguard.h | build
	$(CC) $(RT_CFLAGS) -Isrc $< -o $@

$(KERNEL): $(OBJS) $(EIGS_OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(EIGS_OBJS) -lgcc

verify: $(KERNEL)
	@grub-file --is-x86-multiboot2 $(KERNEL) && echo "OK: valid Multiboot2 kernel" \
		|| { echo "FAIL: not a Multiboot2 kernel"; exit 1; }

$(ISO): $(KERNEL) grub.cfg
	mkdir -p build/iso/boot/grub
	cp $(KERNEL) build/iso/boot/EigenOS.elf
	cp grub.cfg  build/iso/boot/grub/grub.cfg
	grub-mkrescue -d /usr/lib/grub/i386-pc -o $@ build/iso

iso: $(ISO)

# Write the (isohybrid) ISO to a USB stick for real-hardware boot (M7).
# Deliberately refuses to guess the device: OVERWRITES whatever you name.
#   make usb USB=/dev/sdX
usb: $(ISO)
	@if [ -z "$(USB)" ]; then \
		echo "usage: make usb USB=/dev/sdX   (WARNING: overwrites that device)"; \
		echo "hint: the dual-boot path needs no stick — copy $(KERNEL) to the"; \
		echo "      target's /boot and add a 'multiboot2' GRUB menuentry."; \
		exit 1; \
	fi
	sudo dd if=$(ISO) of=$(USB) bs=4M conv=fsync status=progress

run: $(ISO)
	$(QEMU) $(QFLAGS) -cdrom $(ISO)

# Headless boot for no-display dev: VGA still works (QEMU renders it offscreen);
# use `make dump` to read VGA text memory out of the live guest.
run-headless: $(ISO)
	$(QEMU) $(QFLAGS) -cdrom $(ISO) -display none -no-reboot

# Stream the kernel's COM1 serial log to the terminal (the M1 headless
# observation path — no VGA-memory dump needed). Ctrl-A X to quit QEMU.
serial: $(ISO)
	$(QEMU) $(QFLAGS) -cdrom $(ISO) -serial stdio -display none -no-reboot

# M10: a scratch disk for the block layer (raw, 64 MiB = 131072 sectors).
# `make clean` removes it — persistence testing within a session survives
# reboots of the GUEST, which is the contract that matters. QEMU attaches
# it as primary master; -boot d keeps the CD as the boot device.
DISK := build/tidelog.img
DISKARGS := -drive file=$(DISK),format=raw,index=0,media=disk -boot d

$(DISK): | build
	qemu-img create -f raw $(DISK) 64M

disk: $(DISK)

# `make serial` with the scratch disk attached (the M10 dev loop).
serial-disk: $(ISO) $(DISK)
	$(QEMU) $(QFLAGS) -cdrom $(ISO) $(DISKARGS) -serial stdio -display none -no-reboot

# Boot headless, wait for the kernel, dump the first 6 VGA text rows out of
# the live guest via the QEMU monitor, then quit. The 0x-prefixed bytes are
# (char, attr) pairs — chars are every other byte.
dump: $(ISO)
	{ sleep 6; echo "xp /960bx 0xb8000"; sleep 1; echo quit; } | \
		$(QEMU) $(QFLAGS) -cdrom $(ISO) -display none -monitor stdio | grep -a ': 0x'

# Boot headless, screendump the framebuffer once the self-tests are up,
# convert to PNG (the M9.5 visual gate — what the T3200 panel will show).
shot: $(ISO)
	{ sleep 12; echo "screendump build/screen.ppm"; sleep 1; echo quit; } | \
		$(QEMU) $(QFLAGS) -cdrom $(ISO) -display none -monitor stdio >/dev/null
	python3 -c "from PIL import Image; Image.open('build/screen.ppm').save('build/screen.png')"
	@echo "wrote build/screen.png"

clean:
	rm -rf build

.PHONY: all verify iso run run-headless serial disk serial-disk dump shot clean
