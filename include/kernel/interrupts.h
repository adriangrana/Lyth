#ifndef INTERRUPTS_H
#define INTERRUPTS_H

#include <stdint.h>

void interrupts_init_early(void);
void interrupts_init(void);
uintptr_t timer_interrupt_handler(uintptr_t current_rsp);
uintptr_t keyboard_interrupt_handler(uintptr_t current_rsp);
uintptr_t mouse_interrupt_handler(uintptr_t current_rsp);
uintptr_t syscall_interrupt_handler(uintptr_t current_rsp);
uintptr_t exception_interrupt_handler(uintptr_t current_rsp);

#endif