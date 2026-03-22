#include "apic.h"
#include "acpi.h"
#include "paging.h"
#include "klog.h"
#include "idt.h"
#include "gdt.h"

/* ── I/O helpers ────────────────────────────────────────────────── */

static void outb(unsigned short port, unsigned char value) {
	__asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t cpuid_edx_1(void) {
	uint32_t edx;
	__asm__ volatile ("cpuid" : "=d"(edx) : "a"(1U) : "ebx", "ecx");
	return edx;
}

/* ── State ──────────────────────────────────────────────────────── */

static volatile uint32_t* lapic_base;
static volatile uint32_t* ioapic_base;
static int apic_enabled;
static int ioapic_max_redir;

/* ISA→GSI override table (from MADT ISOs) */
#define MAX_ISA_IRQS 16
static uint32_t isa_gsi[MAX_ISA_IRQS];
static uint16_t isa_flags[MAX_ISA_IRQS];

/* ── Local APIC register access ─────────────────────────────────── */

static uint32_t lapic_read(uint32_t offset) {
	return lapic_base[offset / 4];
}

static void lapic_write(uint32_t offset, uint32_t value) {
	lapic_base[offset / 4] = value;
}

/* ── IOAPIC register access ─────────────────────────────────────── */

static uint32_t ioapic_read(uint32_t reg) {
	ioapic_base[0] = reg;         /* IOREGSEL */
	return ioapic_base[4];        /* IOWIN at offset 0x10 */
}

static void ioapic_write(uint32_t reg, uint32_t value) {
	ioapic_base[0] = reg;
	ioapic_base[4] = value;
}

/* ── Spurious interrupt handler (ASM stub calls this) ───────────── */

extern void apic_spurious_stub(void);

/* ── Initialisation ─────────────────────────────────────────────── */

static void pic_disable(void) {
	/* Mask all IRQs on both 8259 PICs */
	outb(0x21, 0xFF);
	outb(0xA1, 0xFF);
}

static void build_iso_table(const acpi_madt_info_t* madt) {
	/* Default: ISA IRQ N → GSI N */
	for (int i = 0; i < MAX_ISA_IRQS; i++) {
		isa_gsi[i] = (uint32_t)i;
		isa_flags[i] = 0;
	}

	/* Apply overrides from MADT */
	for (int i = 0; i < madt->iso_count; i++) {
		uint8_t src = madt->isos[i].source;
		if (src < MAX_ISA_IRQS) {
			isa_gsi[src] = madt->isos[i].gsi;
			isa_flags[src] = madt->isos[i].flags;
		}
	}
}

static void lapic_init(void) {
	/* Set spurious interrupt vector and enable APIC (bit 8) */
	lapic_write(LAPIC_SIVR,
		    APIC_SPURIOUS_VECTOR | (1U << 8));

	/* Clear task priority to accept all interrupts */
	lapic_write(LAPIC_TPR, 0);

	/* Clear error status */
	lapic_write(LAPIC_ESR, 0);
	(void)lapic_read(LAPIC_ESR);
}

static void ioapic_init(void) {
	uint32_t ver = ioapic_read(IOAPIC_REG_VER);
	ioapic_max_redir = (int)((ver >> 16) & 0xFF);

	/* Mask all redirection entries */
	for (int i = 0; i <= ioapic_max_redir; i++) {
		uint32_t reg_lo = IOAPIC_REG_REDIR + (uint32_t)(i * 2);
		uint32_t lo = ioapic_read(reg_lo);
		ioapic_write(reg_lo, lo | (1U << 16));   /* set mask bit */
	}
}

int apic_init(void) {
	const acpi_madt_info_t* madt;

	apic_enabled = 0;

	/* Check CPUID for APIC support (EDX bit 9) */
	if ((cpuid_edx_1() & (1U << 9)) == 0) {
		klog_write(KLOG_LEVEL_WARN, "apic", "CPUID: APIC no soportado");
		return 0;
	}

	/* Need MADT from ACPI */
	madt = acpi_get_madt_info();
	if (!madt->found || madt->ioapic_count == 0) {
		klog_write(KLOG_LEVEL_WARN, "apic", "MADT no disponible o sin IOAPIC");
		return 0;
	}

	/* Map LAPIC MMIO region */
	lapic_base = (volatile uint32_t*)(uintptr_t)madt->lapic_address;
	if (!paging_map_mmio(madt->lapic_address)) {
		klog_write(KLOG_LEVEL_WARN, "apic", "No se pudo mapear LAPIC MMIO");
		return 0;
	}

	/* Map IOAPIC MMIO region */
	ioapic_base = (volatile uint32_t*)(uintptr_t)madt->ioapics[0].ioapic_address;
	if (!paging_map_mmio(madt->ioapics[0].ioapic_address)) {
		klog_write(KLOG_LEVEL_WARN, "apic", "No se pudo mapear IOAPIC MMIO");
		return 0;
	}

	/* Install spurious interrupt handler */
	idt_set_gate(APIC_SPURIOUS_VECTOR,
		     (unsigned int)apic_spurious_stub,
		     gdt_kernel_code_selector(), 0x8E);

	/* Build ISA → GSI mapping */
	build_iso_table(madt);

	/* Disable legacy PICs */
	pic_disable();

	/* Init Local APIC */
	lapic_init();

	/* Init IOAPIC */
	ioapic_init();

	apic_enabled = 1;
	klog_write(KLOG_LEVEL_INFO, "apic", "LAPIC + IOAPIC activos");
	return 1;
}

int apic_is_enabled(void) {
	return apic_enabled;
}

void apic_eoi(void) {
	if (apic_enabled) {
		lapic_write(LAPIC_EOI, 0);
	}
}

/* ── IOAPIC IRQ routing ─────────────────────────────────────────── */

void ioapic_route_irq(uint8_t irq, uint8_t vector, int masked) {
	uint32_t gsi;
	uint16_t flags;
	uint32_t reg_lo;
	uint32_t lo;
	uint32_t hi;

	if (!apic_enabled || irq >= MAX_ISA_IRQS) {
		return;
	}

	gsi = isa_gsi[irq];
	flags = isa_flags[irq];

	if ((int)gsi > ioapic_max_redir) {
		return;
	}

	reg_lo = IOAPIC_REG_REDIR + gsi * 2;

	lo = (uint32_t)vector;

	/* Polarity: bit 1 of flags — 0/1=default(active-high), 3=active-low */
	if ((flags & 0x03) == 0x03) {
		lo |= (1U << 13);   /* active low */
	}

	/* Trigger: bits 2-3 of flags — 0/1=default(edge), 3=level */
	if (((flags >> 2) & 0x03) == 0x03) {
		lo |= (1U << 15);   /* level triggered */
	}

	if (masked) {
		lo |= (1U << 16);
	}

	/* Destination: LAPIC ID 0 (BSP), physical destination mode */
	hi = 0;  /* destination APIC ID = 0 (bits 24-31) */

	ioapic_write(reg_lo + 1, hi);
	ioapic_write(reg_lo, lo);
}

void ioapic_mask_irq(uint8_t irq) {
	uint32_t gsi;
	uint32_t reg_lo;
	uint32_t lo;

	if (!apic_enabled || irq >= MAX_ISA_IRQS) {
		return;
	}

	gsi = isa_gsi[irq];
	if ((int)gsi > ioapic_max_redir) {
		return;
	}

	reg_lo = IOAPIC_REG_REDIR + gsi * 2;
	lo = ioapic_read(reg_lo);
	ioapic_write(reg_lo, lo | (1U << 16));
}

void ioapic_unmask_irq(uint8_t irq) {
	uint32_t gsi;
	uint32_t reg_lo;
	uint32_t lo;

	if (!apic_enabled || irq >= MAX_ISA_IRQS) {
		return;
	}

	gsi = isa_gsi[irq];
	if ((int)gsi > ioapic_max_redir) {
		return;
	}

	reg_lo = IOAPIC_REG_REDIR + gsi * 2;
	lo = ioapic_read(reg_lo);
	ioapic_write(reg_lo, lo & ~(1U << 16));
}

/* ── LAPIC ID + IPI ─────────────────────────────────────────────── */

uint8_t apic_get_id(void) {
	if (!apic_enabled) return 0;
	return (uint8_t)(lapic_read(LAPIC_ID) >> 24);
}

static void lapic_wait_icr(void) {
	/* Wait for delivery status bit (12) to clear */
	while (lapic_read(LAPIC_ICR_LOW) & (1U << 12)) {
		__asm__ volatile ("pause");
	}
}

void apic_send_init(uint8_t dest_lapic_id) {
	if (!apic_enabled) return;
	lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
	/* INIT IPI: delivery mode = 101 (INIT), level assert, edge */
	lapic_write(LAPIC_ICR_LOW, 0x00004500);
	lapic_wait_icr();
}

void apic_send_sipi(uint8_t dest_lapic_id, uint8_t vector) {
	if (!apic_enabled) return;
	lapic_write(LAPIC_ICR_HIGH, (uint32_t)dest_lapic_id << 24);
	/* Startup IPI: delivery mode = 110 (SIPI), vector = page number */
	lapic_write(LAPIC_ICR_LOW, 0x00004600 | (uint32_t)vector);
	lapic_wait_icr();
}

void apic_init_ap(void) {
	/* Each AP initialises its own LAPIC */
	lapic_init();
}
