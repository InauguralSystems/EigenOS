# CPU exception stubs (vectors 0-31) + common dispatch trampoline.
#
# Each stub normalizes the stack to a uniform frame: vectors that don't
# push a hardware error code push a dummy 0 so the layout is identical,
# then every stub pushes its vector number and jumps to isr_common, which
# saves the GP registers, calls the C dispatcher with a pointer to the
# frame, restores, drops (vector, error_code), and iretq's.
.code64
.section .text

.macro ISR_NOERR n
.global isr\n
isr\n:
    pushq $0
    pushq $\n
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    pushq $\n
    jmp isr_common
.endm

ISR_NOERR 0     # #DE divide error
ISR_NOERR 1     # #DB debug
ISR_NOERR 2     # NMI
ISR_NOERR 3     # #BP breakpoint (int3)
ISR_NOERR 4     # #OF overflow
ISR_NOERR 5     # #BR bound range
ISR_NOERR 6     # #UD invalid opcode
ISR_NOERR 7     # #NM device not available
ISR_ERR   8     # #DF double fault
ISR_NOERR 9     # coprocessor segment overrun (legacy)
ISR_ERR   10    # #TS invalid TSS
ISR_ERR   11    # #NP segment not present
ISR_ERR   12    # #SS stack-segment fault
ISR_ERR   13    # #GP general protection
ISR_ERR   14    # #PF page fault
ISR_NOERR 15    # reserved
ISR_NOERR 16    # #MF x87 FP
ISR_ERR   17    # #AC alignment check
ISR_NOERR 18    # #MC machine check
ISR_NOERR 19    # #XM SIMD FP
ISR_NOERR 20    # #VE virtualization
ISR_ERR   21    # #CP control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30    # #SX (treated as no-error here; rarely raised)
ISR_NOERR 31

isr_common:
    push %rax
    push %rbx
    push %rcx
    push %rdx
    push %rsi
    push %rdi
    push %rbp
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov  %rsp, %rdi          # -> interrupt_frame*
    call isr_dispatch
    pop  %r15
    pop  %r14
    pop  %r13
    pop  %r12
    pop  %r11
    pop  %r10
    pop  %r9
    pop  %r8
    pop  %rbp
    pop  %rdi
    pop  %rsi
    pop  %rdx
    pop  %rcx
    pop  %rbx
    pop  %rax
    add  $16, %rsp           # drop vector + error_code
    iretq

.section .rodata
.global isr_stub_table
isr_stub_table:
    .quad isr0,  isr1,  isr2,  isr3,  isr4,  isr5,  isr6,  isr7
    .quad isr8,  isr9,  isr10, isr11, isr12, isr13, isr14, isr15
    .quad isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .quad isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
