#ifndef INTERRUPTS_H
#define INTERRUPTS_H

void interrupts_init(void);
unsigned int timer_interrupt_handler(unsigned int current_esp);
unsigned int keyboard_interrupt_handler(unsigned int current_esp);
unsigned int syscall_interrupt_handler(unsigned int current_esp);

#endif