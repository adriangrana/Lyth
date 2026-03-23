#include "idt.h"
#include <stddef.h>

/* 64-bit IDT entry: 16 bytes each */
struct idt_entry {
    uint16_t base_low;      /* handler offset [15:0] */
    uint16_t selector;      /* code segment selector */
    uint8_t  ist;           /* IST index (bits 0-2), rest 0 */
    uint8_t  flags;         /* type + DPL + P */
    uint16_t base_mid;      /* handler offset [31:16] */
    uint32_t base_high;     /* handler offset [63:32] */
    uint32_t reserved;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idtp;

extern void idt_load(uintptr_t);

void idt_set_gate(unsigned char num, uintptr_t base, unsigned short sel, unsigned char flags) {
    idt[num].base_low  = (uint16_t)(base & 0xFFFFU);
    idt[num].base_mid  = (uint16_t)((base >> 16) & 0xFFFFU);
    idt[num].base_high = (uint32_t)((base >> 32) & 0xFFFFFFFFU);
    idt[num].selector  = sel;
    idt[num].ist       = 0;
    idt[num].flags     = flags;
    idt[num].reserved  = 0;
}

void idt_set_gate_ist(unsigned char num, uintptr_t base, unsigned short sel,
                      unsigned char flags, unsigned char ist) {
    idt_set_gate(num, base, sel, flags);
    idt[num].ist = ist & 0x07;
}

void idt_load_table(void) {
    idt_load((uintptr_t)&idtp);
}

void idt_init(void) {
    idtp.limit = (uint16_t)((sizeof(struct idt_entry) * 256) - 1);
    idtp.base  = (uint64_t)(uintptr_t)&idt;

    for (int i = 0; i < 256; i++) {
        idt_set_gate((unsigned char)i, 0, 0, 0);
    }
}