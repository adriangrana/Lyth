#ifndef IDT_H
#define IDT_H

#include <stdint.h>

void idt_init(void);
void idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags);
void idt_load_table(void);

#endif