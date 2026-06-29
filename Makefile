# EigenOS build. Zero external deps beyond gcc/ld; boot-tests under QEMU.
#   make            build build/EigenOS.elf
#   make verify     assert it's a valid multiboot1 kernel
#   make run        boot it in QEMU (needs qemu-system-x86_64)
#   make clean
CC      := gcc
CFLAGS  := -ffreestanding -nostdlib -m64 -mno-red-zone -mno-mmx -mno-sse \
           -mno-sse2 -Wall -Wextra -Wno-unused-parameter -O2 -c
LDFLAGS := -ffreestanding -nostdlib -no-pie -Wl,-T,linker.ld -Wl,-z,noexecstack \
           -Wl,--build-id=none
QEMU    := qemu-system-x86_64

OBJS := build/boot.o build/kernel.o
KERNEL := build/EigenOS.elf

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
	@grub-file --is-x86-multiboot $(KERNEL) && echo "OK: valid multiboot1 kernel" \
		|| { echo "FAIL: not a multiboot1 kernel"; exit 1; }

run: $(KERNEL)
	$(QEMU) -kernel $(KERNEL)

# Headless boot for CI / no-display environments: serial to stdout, exit on
# triple-fault, 5s cap. (Milestone-0 writes VGA, not serial — this just proves
# it boots without faulting.)
run-headless: $(KERNEL)
	timeout 5 $(QEMU) -kernel $(KERNEL) -display none -no-reboot \
		-d int,guest_errors -serial stdio || true

clean:
	rm -rf build

.PHONY: all verify run run-headless clean
