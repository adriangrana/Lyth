#ifndef APIC_H
#define APIC_H

#include <stdint.h>

/* Local APIC register offsets */
#define LAPIC_ID          0x020
#define LAPIC_VERSION     0x030
#define LAPIC_TPR         0x080
#define LAPIC_EOI         0x0B0
#define LAPIC_SIVR        0x0F0
#define LAPIC_ESR         0x280
#define LAPIC_TIMER_LVT   0x320
#define LAPIC_TIMER_ICR   0x380
#define LAPIC_TIMER_CCR   0x390
#define LAPIC_TIMER_DCR   0x3E0

/* IOAPIC register indices */
#define IOAPIC_REG_ID     0x00
#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_REDIR  0x10

/* Spurious vector */
#define APIC_SPURIOUS_VECTOR  0xFF

int  apic_init(void);
int  apic_is_enabled(void);
void apic_eoi(void);

void ioapic_route_irq(uint8_t irq, uint8_t vector, int masked);
void ioapic_mask_irq(uint8_t irq);
void ioapic_unmask_irq(uint8_t irq);

#endif
