.global idt_load
.global irq0_stub
.global irq1_stub
.global syscall_stub

.extern timer_callback
.extern keyboard_callback
.extern syscall_callback

idt_load:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

irq1_stub:
    pusha
    call keyboard_callback
    popa
    iret

irq0_stub:
    pusha
    call timer_callback
    popa
    iret

syscall_stub:
    push %esi
    push %edx
    push %ecx
    push %ebx
    push %eax
    call syscall_callback
    add $20, %esp
    iret

.section .note.GNU-stack,"",@progbits