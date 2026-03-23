/* 64-bit GDT/TSS management stubs (System V AMD64 ABI) */

.global gdt_flush
.global tss_flush

/* void gdt_flush(uintptr_t gdt_ptr_address);  — RDI = &gdt_ptr */
gdt_flush:
    lgdt    (%rdi)

    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Far return trick: push CS + return address, then lretq */
    lea     .Lgdt_flush_done(%rip), %rax
    push    $0x08           /* kernel code selector */
    push    %rax
    lretq

.Lgdt_flush_done:
    ret

/* void tss_flush(void); */
tss_flush:
    mov     $0x28, %ax
    ltr     %ax
    ret

.section .note.GNU-stack,"",@progbits
