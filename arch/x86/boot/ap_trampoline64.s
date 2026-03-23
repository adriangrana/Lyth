/* ================================================================
 *  AP bootstrap trampoline — 64-bit long mode
 *
 *  Copied to physical address 0x8000 at runtime by smp.c.
 *  The BSP patches the data area before sending the Startup IPI.
 *
 *  Flow:  16-bit real  →  32-bit protected (local 32-bit GDT)
 *         →  enable PAE + LME + paging  →  64-bit long mode
 *         →  reload kernel 64-bit GDT  →  C entry
 *
 *  IMPORTANT: The trampoline carries its own 32-bit GDT for the
 *  real→protected→long transition.  The kernel's 64-bit GDT has
 *  L=1 code segments that cause #GP if used for 32-bit far jumps
 *  on real hardware (QEMU is lenient about this).
 * ================================================================ */

.section .text

.global ap_trampoline_start
.global ap_trampoline_end
.global ap_trampoline_gdt64_desc
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

    /* Load the local 32-bit GDT for the protected-mode transition */
    lgdtl   (0x8000 + (ap_gdt32_desc - ap_trampoline_start))

    /* Enable protected mode (PE bit in CR0) */
    mov     %cr0, %eax
    or      $1, %eax
    mov     %eax, %cr0

    /* Far jump to flush prefetch queue and enter 32-bit code.
       Selector 0x08 → local 32-bit code segment (L=0, D=1). */
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

    /* Now in compatibility mode (32-bit code, paging on, LME set).
       Load a GDT that has a 64-bit code segment at 0x08 for the
       far jump into long mode.  We reuse the same local GDT but
       with a 64-bit code entry at index 3 (selector 0x18). */
    lgdtl   (0x8000 + (ap_gdt32_desc - ap_trampoline_start))

    /* Far jump to 64-bit code via selector 0x18 (64-bit code seg) */
    ljmpl   $0x18, $(0x8000 + (ap_trampoline_lm - ap_trampoline_start))

/* ---- 64-bit long mode ------------------------------------------ */
.code64
ap_trampoline_lm:
    /* Now in full 64-bit long mode.
       Reload the kernel's real GDT (patched by BSP, 10-byte descriptor). */
    lgdt    (0x8000 + (ap_trampoline_gdt64_desc - ap_trampoline_start))

    /* Reload all segment registers with kernel selectors (0x10 = data) */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* Far-return trick to reload CS with kernel code selector 0x08 */
    pushq   $0x08
    leaq    (0x8000 + (.Lreload_cs - ap_trampoline_start))(%rip), %rax
    /* Use absolute address since we know where we are */
    movabs  $(0x8000 + (.Lreload_cs - ap_trampoline_start)), %rax
    pushq   %rax
    lretq

.Lreload_cs:
    /* Load per-AP kernel stack */
    mov     (0x8000 + (ap_trampoline_stack - ap_trampoline_start)), %rsp

    /* Call C entry point (ap_main) */
    mov     (0x8000 + (ap_trampoline_entry - ap_trampoline_start)), %rax
    call    *%rax

    /* Should never return */
    cli
1:  hlt
    jmp     1b

/* ---- Local 32-bit GDT for real→protected→long transition ------- */
.align 16
ap_gdt32:
    .quad   0                       /* 0x00: null descriptor      */
    .quad   0x00CF9A000000FFFF      /* 0x08: 32-bit code (D=1 L=0) */
    .quad   0x00CF92000000FFFF      /* 0x10: 32-bit data          */
    .quad   0x00AF9A000000FFFF      /* 0x18: 64-bit code (L=1 D=0) */
ap_gdt32_end:

ap_gdt32_desc:
    .word   ap_gdt32_end - ap_gdt32 - 1    /* limit */
    .long   0x8000 + (ap_gdt32 - ap_trampoline_start)  /* 32-bit base */

/* ---- Data area (patched by BSP before SIPI) -------------------- */
.align 4
ap_trampoline_gdt64_desc:
    .word   0           /* GDT limit  (patched — kernel's 64-bit GDT) */
    .quad   0           /* GDT base   (patched, 8 bytes for 64-bit)   */

ap_trampoline_stack:
    .quad   0           /* kernel stack top (patched, 64-bit) */

ap_trampoline_cr3:
    .quad   0           /* PML4 physical address (patched, 64-bit) */

ap_trampoline_entry:
    .quad   0           /* C entry point address (patched, 64-bit) */

ap_trampoline_end:

.section .note.GNU-stack,"",@progbits
