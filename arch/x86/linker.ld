OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot ALIGN(4) : {
        KEEP(*(.multiboot))
    } :text

    .text ALIGN(4) : {
        *(.text*)
    } :text

    .rodata ALIGN(4) : {
        *(.rodata*)
    } :text

    .data ALIGN(4) : {
        *(.data*)
    } :data

    .bss ALIGN(4) : {
        *(.bss*)
        *(COMMON)
    } :data

    __kernel_end = .;
}