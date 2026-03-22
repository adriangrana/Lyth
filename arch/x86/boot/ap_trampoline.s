/* ================================================================
 *  AP bootstrap trampoline
 *
 *  Copied to physical address 0x8000 at runtime by smp.c.
 *  The BSP patches the data area (GDT descriptor, stack, CR3,
 *  entry point) before sending the Startup IPI.
 *
 *  Flow:  16-bit real  →  load GDT  →  PE  →  32-bit protected
 *         →  load segments  →  enable PSE+PG  →  call C entry
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

    /* Load kernel page directory */
    mov     (0x8000 + (ap_trampoline_cr3 - ap_trampoline_start)), %eax
    mov     %eax, %cr3

    /* Enable PSE (4 MB pages) */
    mov     %cr4, %eax
    or      $0x10, %eax
    mov     %eax, %cr4

    /* Enable paging */
    mov     %cr0, %eax
    or      $0x80000000, %eax
    mov     %eax, %cr0

    /* Load per-AP kernel stack */
    mov     (0x8000 + (ap_trampoline_stack - ap_trampoline_start)), %esp

    /* Call C entry point (ap_main) */
    mov     (0x8000 + (ap_trampoline_entry - ap_trampoline_start)), %eax
    call    *%eax

    /* Should never return */
    cli
1:  hlt
    jmp     1b

/* ---- Data area (patched by BSP before SIPI) -------------------- */
.align 4
ap_trampoline_gdt_desc:
    .word   0           /* GDT limit  (patched) */
    .long   0           /* GDT base   (patched) */

ap_trampoline_stack:
    .long   0           /* kernel stack top (patched) */

ap_trampoline_cr3:
    .long   0           /* page directory physical address (patched) */

ap_trampoline_entry:
    .long   0           /* C entry point address (patched) */

ap_trampoline_end:

.section .note.GNU-stack,"",@progbits
