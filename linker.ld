/* EigenOS link layout. Loaded at 1 MiB (standard for multiboot kernels — keeps
 * us clear of low memory / BIOS / VGA). The multiboot header must sit in the
 * first 8 KiB of the image, so .multiboot leads. */
ENTRY(_start)

SECTIONS
{
    . = 1M;

    .multiboot : ALIGN(4) { *(.multiboot) }
    .text      : ALIGN(16) { *(.text*) }
    .rodata    : ALIGN(16) { *(.rodata*) }
    .data      : ALIGN(16) { *(.data*) }
    .bss       : ALIGN(16) { *(.bss*) *(COMMON) }

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
