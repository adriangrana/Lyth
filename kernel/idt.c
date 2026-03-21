#include "idt.h"

struct idt_entry {
    unsigned short base_low;
    unsigned short selector;
    unsigned char zero;
    unsigned char flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idt_load(unsigned int);

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_low = (unsigned short)(base & 0xFFFF);
    idt[num].base_high = (unsigned short)((base >> 16) & 0xFFFF);
    idt[num].selector = sel;
    idt[num].zero = 0;
    idt[num].flags = flags;
}

void idt_load_table(void) {
    idt_load((unsigned int)&idtp);
}

void idt_init(void) {
    idtp.limit = (unsigned short)((sizeof(struct idt_entry) * 256) - 1);
    idtp.base = (unsigned int)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }
}