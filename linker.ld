/* EigenOS link layout. Loaded at 1 MiB (standard for multiboot kernels — keeps
 * us clear of low memory / BIOS / VGA). The Multiboot2 header must sit in the
 * first 32768 bytes of the image, 8-byte aligned, so .multiboot leads. */
ENTRY(_start)

SECTIONS
{
    . = 1M;
    _kernel_start = .;


    .multiboot : ALIGN(8) { *(.multiboot) }
    .text      : ALIGN(16) { *(.text*) }
    .rodata    : ALIGN(16) { *(.rodata*) }
    .data      : ALIGN(16) { *(.data*) }

    /* TLS template for the EigenScript runtime's __thread state. Forced to
     * 64-byte alignment so PT_TLS p_align is 64, matching tls_init()'s
     * thread-pointer placement (variant II: TP = block + align_up(size,64)).
     * __tdata_start/__tdata_end bound the init image; __tls_end the whole
     * template (all .tbss here, so the image is empty and the block is
     * just zeroed). */
    . = ALIGN(64);
    .tdata     : ALIGN(64) { __tdata_start = .; *(.tdata .tdata.*) __tdata_end = .; }
    .tbss      : ALIGN(64) { *(.tbss .tbss.*) *(.gnu.linkonce.tb.*) __tls_end = .; }

    .bss       : ALIGN(16) { *(.bss*) *(COMMON) }
    _kernel_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
