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
          build/timer.o build/keyboard.o build/kheap.o build/vga.o
KERNEL := build/EigenOS.elf
ISO    := build/EigenOS.iso

all: $(KERNEL)

build:
	mkdir -p build

build/boot.o: src/boot.S | build
	$(CC) -m64 -c $< -o $@

build/kernel.o: src/kernel.c src/pmm.h src/multiboot2.h src/serial.h src/idt.h src/paging.h src/pic.h src/timer.h src/keyboard.h src/kheap.h src/stackguard.h | build
	$(CC) $(CFLAGS) $< -o $@

build/pmm.o: src/pmm.c src/pmm.h src/multiboot2.h | build
	$(CC) $(CFLAGS) $< -o $@

build/serial.o: src/serial.c src/serial.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/idt.o: src/idt.c src/idt.h src/serial.h src/pic.h src/timer.h src/keyboard.h | build
	$(CC) $(CFLAGS) $< -o $@

build/isr.o: src/isr.S | build
	$(CC) -m64 -c $< -o $@

build/paging.o: src/paging.c src/paging.h src/pmm.h | build
	$(CC) $(CFLAGS) $< -o $@

build/pic.o: src/pic.c src/pic.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/timer.o: src/timer.c src/timer.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/keyboard.o: src/keyboard.c src/keyboard.h src/io.h | build
	$(CC) $(CFLAGS) $< -o $@

build/kheap.o: src/kheap.c src/kheap.h src/pmm.h src/paging.h | build
	$(CC) $(CFLAGS) $< -o $@

build/vga.o: src/vga.c src/vga.h | build
	$(CC) $(CFLAGS) $< -o $@

# EigenScript runtime objects (freestanding + SSE).
build/eigs_%.o: $(EIGS)/src/%.c | build
	$(CC) $(RT_CFLAGS) $< -o $@

build/eigsfs_%.o: $(EIGS)/src/freestanding/%.c | build
	$(CC) $(RT_CFLAGS) $(MINI_CFLAGS) $< -o $@

# The HAL glue: runtime cflags (SSE) so variadic FP + the double return
# from eigs_value_as_num are handled; -Isrc for the kernel headers.
build/hal.o: src/hal.c src/kheap.h src/serial.h src/timer.h src/keyboard.h src/vga.h src/stackguard.h | build
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

run: $(ISO)
	$(QEMU) -cdrom $(ISO)

# Headless boot for no-display dev: VGA still works (QEMU renders it offscreen);
# use `make dump` to read VGA text memory out of the live guest.
run-headless: $(ISO)
	$(QEMU) -cdrom $(ISO) -display none -no-reboot

# Stream the kernel's COM1 serial log to the terminal (the M1 headless
# observation path — no VGA-memory dump needed). Ctrl-A X to quit QEMU.
serial: $(ISO)
	$(QEMU) -cdrom $(ISO) -serial stdio -display none -no-reboot

# Boot headless, wait for the kernel, dump the first 6 VGA text rows out of
# the live guest via the QEMU monitor, then quit. The 0x-prefixed bytes are
# (char, attr) pairs — chars are every other byte.
dump: $(ISO)
	{ sleep 6; echo "xp /960bx 0xb8000"; sleep 1; echo quit; } | \
		$(QEMU) -cdrom $(ISO) -display none -monitor stdio | grep -a ': 0x'

clean:
	rm -rf build

.PHONY: all verify iso run run-headless serial dump clean
