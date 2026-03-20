OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

SECTIONS
{
    . = 1M;
    __kernel_start = .;

    .multiboot ALIGN(4) : {
        KEEP(*(.multiboot))
    }

    .text ALIGN(4) : {
        *(.text*)
    }

    .rodata ALIGN(4) : {
        *(.rodata*)
    }

    .data ALIGN(4) : {
        *(.data*)
    }

    .bss ALIGN(4) : {
        *(.bss*)
        *(COMMON)
    }

    __kernel_end = .;
}