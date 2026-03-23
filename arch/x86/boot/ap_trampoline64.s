/* ================================================================
 *  AP bootstrap trampoline — 64-bit long mode
 *
 *  Copied to physical address 0x8000 at runtime by smp.c.
 *  The BSP patches the data area before sending the Startup IPI.
 *
 *  Flow:  16-bit real  →  32-bit protected  →  enable PAE + LME
 *         →  enable paging  →  64-bit long mode  →  C entry
 * ================================================================ */

.section .text

.global ap_trampoline_start
.global ap_trampoline_end
.global ap_trampoline_gdt_desc
.global ap_trampoline_stack
.global ap_trampoline_cr3
.global ap_trampoline_entry

ap_trampoline_start:

/* ---- 16-bit real-mode entry ------------------------------------ */
.code16
    cli
    xor     %ax, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %ss

    /* Load GDT from patched descriptor (at known physical offset) */
    lgdtl   (0x8000 + (ap_trampoline_gdt_desc - ap_trampoline_start))

    /* Enable protected mode (PE bit in CR0) */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    /* Far jump to flush prefetch queue and enter 32-bit code */
    ljmpl   $0x08, $(0x8000 + (ap_trampoline_pm - ap_trampoline_start))

/* ---- 32-bit protected mode ------------------------------------- */
.code32
ap_trampoline_pm:
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Load PML4 from patched field */
    mov     (0x8000 + (ap_trampoline_cr3 - ap_trampoline_start)), %eax
    mov     %eax, %cr3

    /* Enable PAE (CR4.PAE = bit 5) */
    mov     %cr4, %eax
    or      $(1 << 5), %eax
    mov     %eax, %cr4

    /* Set EFER.LME (Long Mode Enable, MSR 0xC0000080, bit 8) */
    mov     $0xC0000080, %ecx
    rdmsr
    or      $(1 << 8), %eax
    wrmsr

    /* Enable paging + write protect */
    mov     %cr0, %eax
    or      $0x80010000, %eax
    mov     %eax, %cr0

    /* Reload GDT (needed for 64-bit segments) */
    lgdtl   (0x8000 + (ap_trampoline_gdt_desc - ap_trampoline_start))

    /* Far jump to 64-bit code */
    ljmpl   $0x08, $(0x8000 + (ap_trampoline_lm - ap_trampoline_start))

/* ---- 64-bit long mode ------------------------------------------ */
.code64
ap_trampoline_lm:
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Load per-AP kernel stack */
    mov     (0x8000 + (ap_trampoline_stack - ap_trampoline_start)), %rsp

    /* Call C entry point (ap_main) */
    mov     (0x8000 + (ap_trampoline_entry - ap_trampoline_start)), %rax
    call    *%rax

    /* Should never return */
    cli
1:  hlt
    jmp     1b

/* ---- Data area (patched by BSP before SIPI) -------------------- */
.align 4
ap_trampoline_gdt_desc:
    .word   0           /* GDT limit  (patched) */
    .quad   0           /* GDT base   (patched, 8 bytes for 64-bit) */

ap_trampoline_stack:
    .quad   0           /* kernel stack top (patched, 64-bit) */

ap_trampoline_cr3:
    .quad   0           /* PML4 physical address (patched, 64-bit) */

ap_trampoline_entry:
    .quad   0           /* C entry point address (patched, 64-bit) */

ap_trampoline_end:

.section .note.GNU-stack,"",@progbits
