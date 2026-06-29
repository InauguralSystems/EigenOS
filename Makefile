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

OBJS   := build/boot.o build/kernel.o
KERNEL := build/EigenOS.elf
ISO    := build/EigenOS.iso

all: $(KERNEL)

build:
	mkdir -p build

build/boot.o: src/boot.S | build
	$(CC) -m64 -c $< -o $@

build/kernel.o: src/kernel.c | build
	$(CC) $(CFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

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

clean:
	rm -rf build

.PHONY: all verify iso run run-headless clean
