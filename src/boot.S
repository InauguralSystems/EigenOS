# EigenOS boot stub.
#
# GRUB (Multiboot2) loads this ELF64 and enters in 32-bit protected mode, paging
# off (ebx = multiboot2 info ptr). We identity-map the first 1 GiB with 2 MiB
# pages,
# turn on PAE + long mode + paging, load a 64-bit GDT, and far-jump into 64-bit
# code, which hands off to kernel_main(). The kernel ELF is ELF64 but this
# entry section is assembled .code32 because the CPU is still 32-bit here.

# Multiboot2 header (GRUB loads ELF64 directly; magic 0xE85250D6, arch 0=i386
# protected-mode entry). GRUB drops us in 32-bit protected mode just like MB1,
# so the long-mode bring-up below is unchanged.
.set MB2_MAGIC, 0xE85250D6
.set MB2_ARCH,  0

.section .multiboot, "a"
.align 8
mb2_start:
    .long MB2_MAGIC
    .long MB2_ARCH
    .long mb2_end - mb2_start
    .long -(MB2_MAGIC + MB2_ARCH + (mb2_end - mb2_start))
    .align 8                                 # end tag: type=0, flags=0, size=8
    .word 0
    .word 0
    .long 8
mb2_end:

.section .bss, "aw", @nobits
.align 16
stack_bottom:
.skip 65536                                  # 64 KiB boot stack (runtime recursion)
stack_top:

.align 4096                                  # page tables (must be 4 KiB aligned)
pml4: .skip 4096
pdpt: .skip 4096
pd:   .skip 4096

.section .rodata
.align 8
gdt64:
    .quad 0                                  # null descriptor
    .set GDT_CODE, . - gdt64
    .quad 0x00209A0000000000                 # 64-bit code: P, DPL0, exec, L=1
    .set GDT_DATA, . - gdt64
    .quad 0x0000920000000000                 # data: P, DPL0, writable
gdt64_pointer:
    .word . - gdt64 - 1
    .quad gdt64

.section .text
.code32
.global _start
.type _start, @function
_start:
    mov   $stack_top, %esp                   # multiboot gives no usable stack
    mov   %eax, %ebp                         # save MB2 magic; eax is clobbered below
                                             # (ebx = MB2 info ptr survives untouched)

    # --- page tables: identity-map 0..1GiB with 2 MiB pages ---
    mov   $pdpt, %eax
    or    $0x3, %eax                          # present | writable
    mov   %eax, pml4                          # pml4[0] -> pdpt

    mov   $pd, %eax
    or    $0x3, %eax
    mov   %eax, pdpt                          # pdpt[0] -> pd

    mov   $pd, %edi
    mov   $0x83, %eax                         # present | writable | huge, addr=0
    mov   $512, %ecx
.fill_pd:
    mov   %eax, (%edi)                        # low dword: addr | flags
    movl  $0, 4(%edi)                         # high dword: 0
    add   $0x200000, %eax                     # next 2 MiB
    add   $8, %edi
    loop  .fill_pd

    mov   $pml4, %eax
    mov   %eax, %cr3                           # CR3 = PML4

    mov   %cr4, %eax
    or    $(1 << 5), %eax                      # CR4.PAE
    mov   %eax, %cr4

    mov   $0xC0000080, %ecx                    # EFER MSR
    rdmsr
    or    $(1 << 8), %eax                      # EFER.LME (long mode enable)
    wrmsr

    mov   %cr0, %eax
    or    $(1 << 31), %eax                     # CR0.PG (paging) -> long mode active
    mov   %eax, %cr0

    lgdt  gdt64_pointer
    ljmp  $GDT_CODE, $long_mode_start          # far jump into 64-bit code

.code64
long_mode_start:
    mov   $GDT_DATA, %ax
    mov   %ax, %ds
    mov   %ax, %es
    mov   %ax, %fs
    mov   %ax, %gs
    mov   %ax, %ss
    mov   $stack_top, %rsp

    # Enable SSE: the EigenScript runtime is all doubles, and x86-64 passes
    # them in xmm registers. The kernel core is -mno-sse, but the runtime
    # objects are built -msse2, so turn the FPU/SSE units on here (once, for
    # the whole machine). CR0: clear EM (bit2), set MP (bit1). CR4: set
    # OSFXSR (bit9) + OSXMMEXCPT (bit10).
    mov   %cr0, %rax
    and   $~(1 << 2), %rax
    or    $(1 << 1), %rax
    mov   %rax, %cr0
    mov   %cr4, %rax
    or    $(3 << 9), %rax
    mov   %rax, %cr4

    # SysV handoff: GRUB's Multiboot2 info pointer and magic become
    # kernel_main(info, magic). The 32-bit moves zero-extend into rdi/rsi;
    # GRUB places the info struct below 4 GiB, inside our identity map.
    mov   %ebx, %edi
    mov   %ebp, %esi
    call  kernel_main                          # never returns
.hang:
    cli
    hlt
    jmp   .hang

.size _start, . - _start
