/* =================================================================
 *  Lyth OS — 64-bit boot stub
 *
 *  GRUB delivers us in 32-bit protected mode via Multiboot.
 *  We set up identity-mapped 4-level page tables, enable long mode,
 *  and far-jump to the 64-bit kernel entry.
 *
 *  Flow:  32-bit protected  →  enable PAE + PML4  →  set EFER.LME
 *         →  enable paging  →  far jump to 64-bit  →  kernel_main
 * ================================================================= */

/* ---- Multiboot header (must be in first 8 KB) -------------------- */
.section .multiboot, "a"
.align 4
.set MB_MAGIC,    0x1BADB002
.set MB_FLAGS,    0x6           /* bit 1: memory info, bit 2: video mode */
.set MB_CHECKSUM, -(MB_MAGIC + MB_FLAGS)

.long MB_MAGIC
.long MB_FLAGS
.long MB_CHECKSUM
.long 0, 0, 0, 0, 0            /* aout kludge (unused for ELF) */
.long 0                         /* mode_type: linear */
.long 1024                      /* width  — widely supported */
.long 768                       /* height — widely supported */
.long 32                        /* depth */

/* ---- BSS: kernel stack ------------------------------------------- */
.section .bss
.align 16
stack_bottom:
.skip 32768                     /* 32 KB kernel stack */
stack_top:

/* ---- Boot page tables (identity map first 4 GB with 2 MB pages) -- */
.section .bss
.align 4096
.global boot_pml4
boot_pml4:   .skip 4096
boot_pdpt:   .skip 4096
boot_pd0:    .skip 4096         /* 0 GB – 1 GB */
boot_pd1:    .skip 4096         /* 1 GB – 2 GB */
boot_pd2:    .skip 4096         /* 2 GB – 3 GB */
boot_pd3:    .skip 4096         /* 3 GB – 4 GB */

/* ---- Early-map page-table pool (for framebuffer above 4 GB) ------ */
.section .bss
.align 4096
.global early_extra_pds
early_extra_pds:  .skip 4096 * 16    /* 16 PD tables for dynamic mapping   */
.global early_extra_pdpt
early_extra_pdpt: .skip 4096          /* 1 extra PDPT (PML4 entry != 0)    */

/* ---- saved multiboot pointer ------------------------------------- */
.section .data
.global saved_multiboot_ptr
saved_multiboot_ptr:
    .long 0

/* ---- 64-bit GDT for the transition ------------------------------ */
.section .rodata
.align 16
.global gdt64
gdt64:
    .quad 0                     /* 0x00: null descriptor */
    .quad 0x00AF9A000000FFFF    /* 0x08: 64-bit kernel code, DPL 0, L=1 D=0 */
    .quad 0x00CF92000000FFFF    /* 0x10: 64-bit kernel data, DPL 0 */
    .quad 0x00AFFA000000FFFF    /* 0x18: 64-bit user code, DPL 3, L=1 D=0 */
    .quad 0x00CFF2000000FFFF    /* 0x20: 64-bit user data, DPL 3 */
.global gdt64_tss
gdt64_tss:
    .quad 0                     /* 0x28: TSS low  (filled by gdt_init) */
    .quad 0                     /* 0x30: TSS high (filled by gdt_init) */
.global gdt64_end
gdt64_end:

.global gdt64_pointer
gdt64_pointer:
    .word gdt64_end - gdt64 - 1
    .quad gdt64

/* ---- 32-bit code: Multiboot entry point -------------------------- */
.section .text
.code32
.global _start
.extern kernel_main

_start:
    cli

    /* Save multiboot info pointer (EBX) to memory immediately */
    mov     %ebx, saved_multiboot_ptr

    /* ---- Build identity-mapped page tables ---- */

    /* Zero all 6 page tables (PML4 + PDPT + 4×PD) */
    mov     $boot_pml4, %edi
    xor     %eax, %eax
    mov     $(4096 * 6 / 4), %ecx  /* 6 pages, dword count */
    rep stosl

    /* PML4[0] → PDPT */
    mov     $boot_pdpt, %eax
    or      $0x03, %eax             /* present + writable */
    mov     %eax, boot_pml4

    /* PDPT[0..3] → PD0..PD3 */
    mov     $boot_pd0, %eax
    or      $0x03, %eax
    mov     %eax, boot_pdpt + 0
    mov     $boot_pd1, %eax
    or      $0x03, %eax
    mov     %eax, boot_pdpt + 8
    mov     $boot_pd2, %eax
    or      $0x03, %eax
    mov     %eax, boot_pdpt + 16
    mov     $boot_pd3, %eax
    or      $0x03, %eax
    mov     %eax, boot_pdpt + 24

    /* Fill each PD with 512 × 2 MB pages (total 4 × 1 GB = 4 GB) */
    mov     $boot_pd0, %edi
    mov     $0, %ebx                /* physical address counter */
    mov     $(512 * 4), %ecx        /* 2048 entries total */
1:
    mov     %ebx, %eax
    or      $0x83, %eax             /* present + writable + PS (2 MB) */
    mov     %eax, (%edi)
    movl    $0, 4(%edi)             /* high 32 bits = 0 */
    add     $0x200000, %ebx         /* next 2 MB */
    add     $8, %edi
    dec     %ecx
    jnz     1b

    /* ---- Enable long mode ---- */

    /* Load PML4 into CR3 */
    mov     $boot_pml4, %eax
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

    /* Enable paging + write protect (CR0.PG bit 31, CR0.WP bit 16) */
    mov     %cr0, %eax
    or      $0x80010000, %eax
    mov     %eax, %cr0

    /* Load 64-bit GDT */
    lgdt    gdt64_pointer

    /* Far jump to 64-bit code segment to enter long mode */
    ljmp    $0x08, $long_mode_entry

/* ---- 64-bit code: long mode entry -------------------------------- */
.code64
long_mode_entry:
    /* Load 64-bit data segments */
    mov     $0x10, %ax
    mov     %ax, %ds
    mov     %ax, %es
    mov     %ax, %fs
    mov     %ax, %gs
    mov     %ax, %ss

    /* ---- Program PAT MSR for Write-Combining support ----
     * Default PAT: 0=WB 1=WT 2=UC- 3=UC 4=WB 5=WT 6=UC- 7=UC
     * We change entry 1 (and 5) from WT to WC so that any page
     * with PWT=1 gets Write-Combining.  Existing mappings use
     * PWT=0, so they keep WB (entry 0) and are unaffected.
     *   EAX = PA3:PA2:PA1:PA0 = 00:07:01:06
     *   EDX = PA7:PA6:PA5:PA4 = 00:07:01:06
     * PAT MSR = 0x277 */
    mov     $0x277, %ecx
    mov     $0x00070106, %eax
    mov     $0x00070106, %edx
    wrmsr

    /* Set up 64-bit stack */
    lea     stack_top(%rip), %rsp

    /* Load saved multiboot pointer into RDI (1st arg, SysV AMD64 ABI) */
    lea     saved_multiboot_ptr(%rip), %rax
    movl    (%rax), %edi            /* zero-extends to 64 bits */

    /* Ensure 16-byte stack alignment before call */
    and     $~0xF, %rsp

    /* Call kernel_main(multiboot_info_t*) */
    call    kernel_main

    /* Should never return */
    cli
2:  hlt
    jmp     2b

.section .note.GNU-stack,"",@progbits
