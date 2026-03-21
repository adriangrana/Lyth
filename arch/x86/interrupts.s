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
.global irq12_stub
.global syscall_stub

.extern exception_interrupt_handler
.extern timer_interrupt_handler
.extern keyboard_interrupt_handler
.extern mouse_interrupt_handler
.extern syscall_interrupt_handler

.macro ISR_NOERR num
isr\num\()_stub:
    push $0
    push $\num
    jmp exception_common_stub
.endm

.macro ISR_ERR num
isr\num\()_stub:
    push $\num
    jmp exception_common_stub
.endm

idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

exception_common_stub:
    pusha
    mov %esp, %eax
    push %eax
    call exception_interrupt_handler
    add $4, %esp
    mov %eax, %esp
    popa
    add $8, %esp
    iret

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

irq1_stub:
    pusha
    mov %esp, %eax
    push %eax
    call keyboard_interrupt_handler
    add $4, %esp
    mov %eax, %esp
    popa
    iret

irq0_stub:
    pusha
    mov %esp, %eax
    push %eax
    call timer_interrupt_handler
    add $4, %esp
    mov %eax, %esp
    popa
    iret

irq12_stub:
    pusha
    mov %esp, %eax
    push %eax
    call mouse_interrupt_handler
    add $4, %esp
    mov %eax, %esp
    popa
    iret

syscall_stub:
    pusha
    mov %esp, %eax
    push %eax
    call syscall_interrupt_handler
    add $4, %esp
    mov %eax, %esp
    popa
    iret

.section .note.GNU-stack,"",@progbits
