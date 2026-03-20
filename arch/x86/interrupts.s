.global idt_load
.global irq0_stub
.global irq1_stub
.global syscall_stub

.extern timer_interrupt_handler
.extern keyboard_interrupt_handler
.extern syscall_interrupt_handler

idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

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