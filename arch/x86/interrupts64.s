/* =================================================================
 *  Lyth OS — 64-bit interrupt stubs
 *
 *  x86_64 has no pusha/popa.  We manually save/restore all 15 GPRs
 *  (RAX–R15, excluding RSP which is in the CPU-pushed frame).
 *
 *  Stack frame after common stub entry (low → high):
 *    r15 r14 r13 r12 r11 r10 r9 r8
 *    rdi rsi rbp rbx rdx rcx rax
 *    vector  error_code
 *    rip  cs  rflags  rsp  ss       ← CPU-pushed
 *
 *  The C handler receives RSP (pointer to frame) in RDI and
 *  returns the (possibly new) RSP in RAX.
 * ================================================================= */

.global idt_load
.global isr0_stub
.global isr1_stub
.global isr2_stub
.global isr3_stub
.global isr4_stub
.global isr5_stub
.global isr6_stub
.global isr7_stub
.global isr8_stub
.global isr9_stub
.global isr10_stub
.global isr11_stub
.global isr12_stub
.global isr13_stub
.global isr14_stub
.global isr15_stub
.global isr16_stub
.global isr17_stub
.global isr18_stub
.global isr19_stub
.global isr20_stub
.global isr21_stub
.global isr22_stub
.global isr23_stub
.global isr24_stub
.global isr25_stub
.global isr26_stub
.global isr27_stub
.global isr28_stub
.global isr29_stub
.global isr30_stub
.global isr31_stub
.global irq0_stub
.global irq1_stub
.global irq11_stub
.global irq12_stub
.global irq14_stub
.global syscall_stub
.global apic_spurious_stub

.extern exception_interrupt_handler
.extern timer_interrupt_handler
.extern keyboard_interrupt_handler
.extern e1000_interrupt_handler
.extern mouse_interrupt_handler
.extern syscall_interrupt_handler
.extern ata_irq14_handler

/* void idt_load(uintptr_t idt_ptr_address);  — RDI = &idtp */
idt_load:
    lidt    (%rdi)
    ret

/* ── Save / restore macros ────────────────────────────────────────── */

.macro SAVE_ALL
    push    %rax
    push    %rcx
    push    %rdx
    push    %rbx
    push    %rbp
    push    %rsi
    push    %rdi
    push    %r8
    push    %r9
    push    %r10
    push    %r11
    push    %r12
    push    %r13
    push    %r14
    push    %r15
.endm

.macro RESTORE_ALL
    pop     %r15
    pop     %r14
    pop     %r13
    pop     %r12
    pop     %r11
    pop     %r10
    pop     %r9
    pop     %r8
    pop     %rdi
    pop     %rsi
    pop     %rbp
    pop     %rbx
    pop     %rdx
    pop     %rcx
    pop     %rax
.endm

/* ── ISR macros ──────────────────────────────────────────────────── */

.macro ISR_NOERR num
isr\num\()_stub:
    push    $0              /* dummy error code */
    push    $\num           /* vector number */
    jmp     exception_common_stub
.endm

.macro ISR_ERR num
isr\num\()_stub:
    /* error code already pushed by CPU */
    push    $\num           /* vector number */
    jmp     exception_common_stub
.endm

/* ── Exception common path ───────────────────────────────────────── */
exception_common_stub:
    SAVE_ALL
    mov     %rsp, %rdi              /* arg0 = frame pointer */
    call    exception_interrupt_handler
    mov     %rax, %rsp              /* handler may return new RSP */
    RESTORE_ALL
    add     $16, %rsp               /* pop vector + error_code */
    iretq

/* ── ISR entries (exceptions 0-31) ────────────────────────────────── */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_ERR   30
ISR_NOERR 31

/* ── IRQ stubs ───────────────────────────────────────────────────── */
/* All IRQs push a dummy error_code and vector to keep the frame
   layout identical to exceptions (so context-switch stacks match). */

/* IRQ 0 — PIT / HPET timer */
irq0_stub:
    push    $0              /* dummy error code */
    push    $32             /* vector = 32 (IRQ0) */
    SAVE_ALL
    mov     %rsp, %rdi
    call    timer_interrupt_handler
    mov     %rax, %rsp
    RESTORE_ALL
    add     $16, %rsp       /* skip vector + error_code */
    iretq

/* IRQ 1 — keyboard */
irq1_stub:
    push    $0
    push    $33
    SAVE_ALL
    mov     %rsp, %rdi
    call    keyboard_interrupt_handler
    mov     %rax, %rsp
    RESTORE_ALL
    add     $16, %rsp
    iretq

/* IRQ 12 — PS/2 mouse */
irq12_stub:
    push    $0
    push    $44
    SAVE_ALL
    mov     %rsp, %rdi
    call    mouse_interrupt_handler
    mov     %rax, %rsp
    RESTORE_ALL
    add     $16, %rsp
    iretq

/* IRQ 14 — ATA primary */
irq14_stub:
    push    $0
    push    $46
    SAVE_ALL
    mov     %rsp, %rdi
    call    ata_irq14_handler
    mov     %rax, %rsp
    RESTORE_ALL
    add     $16, %rsp
    iretq

/* IRQ 11 — E1000 NIC */
irq11_stub:
    push    $0
    push    $43
    SAVE_ALL
    call    e1000_interrupt_handler
    RESTORE_ALL
    add     $16, %rsp
    iretq

/* INT 0x80 — syscall */
syscall_stub:
    push    $0
    push    $0x80
    SAVE_ALL
    mov     %rsp, %rdi
    call    syscall_interrupt_handler
    mov     %rax, %rsp
    RESTORE_ALL
    add     $16, %rsp
    iretq

/* APIC spurious */
apic_spurious_stub:
    iretq

.section .note.GNU-stack,"",@progbits
