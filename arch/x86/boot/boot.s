.section .multiboot, "a"
.align 4
.set MAGIC, 0x1BADB002
.set FLAGS, 0x4
.set CHECKSUM, -(MAGIC + FLAGS)

.long MAGIC
.long FLAGS
.long CHECKSUM
.long 0
.long 0
.long 0
.long 0
.long 0
.long 0
.long 1280
.long 1024
.long 32

.section .bss
.align 16
stack_bottom:
.skip 16384
stack_top:

.section .text
.global _start
.extern kernel_main

_start:
    mov $stack_top, %esp
    /* Multiboot: GRUB passes pointer in EBX; forward it to kernel_main */
    push %ebx
    call kernel_main

hang:
    cli
    hlt
    jmp hang

.section .note.GNU-stack,"",@progbits
