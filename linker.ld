/* EigenOS link layout. Loaded at 1 MiB (standard for multiboot kernels — keeps
 * us clear of low memory / BIOS / VGA). The Multiboot2 header must sit in the
 * first 32768 bytes of the image, 8-byte aligned, so .multiboot leads. */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(8) { *(.multiboot) }
    .text      : ALIGN(16) { *(.text*) }
    .rodata    : ALIGN(16) { *(.rodata*) }
    .data      : ALIGN(16) { *(.data*) }
    .bss       : ALIGN(16) { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
