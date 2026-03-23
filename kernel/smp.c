#include "smp.h"
#include "acpi.h"
#include "apic.h"
#include "gdt.h"
#include "idt.h"
#include "paging.h"
#include "klog.h"

/* ── Trampoline symbols (defined in ap_trampoline64.s) ────────────── */

extern uint8_t  ap_trampoline_start[];
extern uint8_t  ap_trampoline_end[];
extern uint16_t ap_trampoline_gdt_desc;   /* word limit + quad base (10 bytes) */
extern uint64_t ap_trampoline_stack;
extern uint64_t ap_trampoline_cr3;
extern uint64_t ap_trampoline_entry;

/* ── Constants ──────────────────────────────────────────────────── */

#define AP_TRAMPOLINE_PHYS   0x8000      /* physical page for trampoline */
#define AP_TRAMPOLINE_VECTOR (AP_TRAMPOLINE_PHYS >> 12)   /* SIPI vector */

#define AP_STACK_SIZE        4096

/* ── Per-CPU state ──────────────────────────────────────────────── */

static uint8_t  ap_stacks[SMP_MAX_CPUS][AP_STACK_SIZE]
				__attribute__((aligned(16)));

static volatile int ap_ready;      /* AP sets this to 1 once booted */

static int       cpu_count;
static uint8_t   bsp_lapic_id;

/* ── Tiny delay (PIT-based) ─────────────────────────────────────── */

static void outb(uint16_t port, uint8_t val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static uint8_t inb(uint16_t port) {
	uint8_t v;
	__asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

/* Approximate busy-wait using PIT channel 2.
   Waits at least `us` microseconds (~1.193 MHz tick). */
static void delay_us(uint32_t us) {
	uint32_t ticks = (us * 1193U) / 1000U;
	if (ticks == 0) ticks = 1;
	if (ticks > 0xFFFF) ticks = 0xFFFF;

	/* Gate off, speaker off */
	uint8_t ctrl = (inb(0x61) & 0xFC) | 0x01;
	outb(0x61, ctrl);

	/* PIT channel 2, mode 0 (one-shot), lobyte/hibyte */
	outb(0x43, 0xB0);
	outb(0x42, (uint8_t)(ticks & 0xFF));
	outb(0x42, (uint8_t)((ticks >> 8) & 0xFF));

	/* Reset latch and wait for OUT pin (bit 5 of port 0x61) */
	uint8_t tmp = inb(0x61) & 0xFE;
	outb(0x61, tmp);
	outb(0x61, tmp | 0x01);

	while ((inb(0x61) & 0x20) == 0)
		__asm__ volatile ("pause");
}

/* ── AP C entry point ───────────────────────────────────────────── */

static void ap_main(void);

static void ap_main(void) {
	/* Load per-CPU GDT + TSS with our own kernel stack */
	uintptr_t my_stack_top;
	__asm__ volatile ("mov %%rsp, %0" : "=r"(my_stack_top));
	gdt_init_ap(my_stack_top);

	/* Load shared IDT */
	idt_load_table();

	/* Initialise this core's Local APIC */
	apic_init_ap();

	/* Signal BSP that we are online */
	__sync_synchronize();
	ap_ready = 1;

	/* Halt loop — APs do not participate in scheduling yet */
	for (;;)
		__asm__ volatile ("hlt");
}

/* ── Trampoline setup ───────────────────────────────────────────── */

static void install_trampoline(uintptr_t stack_top) {
	uint8_t* dest = (uint8_t*)(uintptr_t)AP_TRAMPOLINE_PHYS;
	uint32_t size = (uint32_t)(ap_trampoline_end - ap_trampoline_start);

	/* Copy trampoline code to low memory */
	for (uint32_t i = 0; i < size; i++)
		dest[i] = ap_trampoline_start[i];

	/* Patch data fields.
	   Offsets are relative to ap_trampoline_start, relocated to phys base. */
	uint32_t off_gdt   = (uint32_t)((uint8_t*)&ap_trampoline_gdt_desc - ap_trampoline_start);
	uint32_t off_stack = (uint32_t)((uint8_t*)&ap_trampoline_stack    - ap_trampoline_start);
	uint32_t off_cr3   = (uint32_t)((uint8_t*)&ap_trampoline_cr3     - ap_trampoline_start);
	uint32_t off_entry = (uint32_t)((uint8_t*)&ap_trampoline_entry   - ap_trampoline_start);

	/* GDT descriptor: reuse BSP's loaded GDTR (kernel page-dir identity maps it)
	 * 64-bit GDTR: 2-byte limit + 8-byte base */
	struct { uint16_t limit; uint64_t base; } __attribute__((packed)) gdtr;
	__asm__ volatile ("sgdt %0" : "=m"(gdtr));
	*(uint16_t*)(dest + off_gdt)     = gdtr.limit;
	*(uint64_t*)(dest + off_gdt + 2) = gdtr.base;

	*(uint64_t*)(dest + off_stack) = (uint64_t)stack_top;

	/* CR3: PML4 physical address */
	uint64_t cr3;
	__asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
	*(uint64_t*)(dest + off_cr3) = cr3;

	*(uint64_t*)(dest + off_entry) = (uint64_t)(uintptr_t)ap_main;
}

/* ── Public API ─────────────────────────────────────────────────── */

void smp_init(void) {
	const acpi_madt_info_t* madt;

	cpu_count = 1;    /* BSP is always CPU 0 */

	if (!apic_is_enabled()) {
		klog_write(KLOG_LEVEL_INFO, "smp", "APIC desactivado, solo BSP");
		return;
	}

	bsp_lapic_id = apic_get_id();

	madt = acpi_get_madt_info();
	if (!madt->found || madt->lapic_count == 0) {
		klog_write(KLOG_LEVEL_INFO, "smp", "Sin info LAPIC en MADT");
		return;
	}

	int total_cpus = 0;
	for (int i = 0; i < madt->lapic_count; i++) {
		if (madt->lapics[i].flags & 1)
			total_cpus++;
	}

	if (total_cpus <= 1) {
		klog_write(KLOG_LEVEL_INFO, "smp", "Solo hay 1 CPU");
		return;
	}

	klog_write(KLOG_LEVEL_INFO, "smp", "Detectadas CPUs (MADT):");

	/* Boot each AP */
	for (int i = 0; i < madt->lapic_count; i++) {
		uint8_t lapic_id = madt->lapics[i].lapic_id;

		/* Skip disabled processors */
		if (!(madt->lapics[i].flags & 1))
			continue;

		/* Skip BSP itself */
		if (lapic_id == bsp_lapic_id)
			continue;

		if (cpu_count >= SMP_MAX_CPUS)
			break;

		/* Prepare per-AP stack */
		uintptr_t stack_top = (uintptr_t)
			(ap_stacks[cpu_count] + AP_STACK_SIZE);

		/* Install trampoline with this AP's stack */
		install_trampoline(stack_top);

		ap_ready = 0;
		__sync_synchronize();

		/* INIT – SIPI – SIPI sequence */
		apic_send_init(lapic_id);
		delay_us(10000);             /* 10 ms */

		apic_send_sipi(lapic_id, AP_TRAMPOLINE_VECTOR);
		delay_us(200);               /* 200 μs */

		if (!ap_ready) {
			apic_send_sipi(lapic_id, AP_TRAMPOLINE_VECTOR);
			delay_us(200);
		}

		/* Wait up to ~100 ms for AP to report */
		for (int w = 0; w < 100 && !ap_ready; w++)
			delay_us(1000);

		if (ap_ready) {
			cpu_count++;
			klog_write(KLOG_LEVEL_INFO, "smp", "  AP online (LAPIC)");
		} else {
			klog_write(KLOG_LEVEL_WARN, "smp", "  AP no responde");
		}
	}

	klog_write(KLOG_LEVEL_INFO, "smp", "SMP listo");
}

int smp_cpu_count(void) {
	return cpu_count;
}

uint8_t smp_bsp_id(void) {
	return bsp_lapic_id;
}
