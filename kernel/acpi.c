#include "acpi.h"
#include "klog.h"

/* ── Inline port I/O helpers ────────────────────────────────────── */

static inline void outb(uint16_t port, uint8_t val) {
	__asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline void outw(uint16_t port, uint16_t val) {
	__asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
	uint16_t val;
	__asm__ volatile ("inw %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

/* ── ACPI table structures ──────────────────────────────────────── */

typedef struct {
	char     signature[8];
	uint8_t  checksum;
	char     oem_id[6];
	uint8_t  revision;
	uint32_t rsdt_address;
} __attribute__((packed)) rsdp_t;

typedef struct {
	char     signature[4];
	uint32_t length;
	uint8_t  revision;
	uint8_t  checksum;
	char     oem_id[6];
	char     oem_table_id[8];
	uint32_t oem_revision;
	uint32_t creator_id;
	uint32_t creator_revision;
} __attribute__((packed)) acpi_sdt_header_t;

typedef struct {
	acpi_sdt_header_t header;
	uint32_t          entries[];
} __attribute__((packed)) rsdt_t;

typedef struct {
	acpi_sdt_header_t header;
	uint32_t          lapic_address;
	uint32_t          flags;
} __attribute__((packed)) madt_header_t;

typedef struct {
	uint8_t  type;
	uint8_t  length;
} __attribute__((packed)) madt_entry_header_t;

typedef struct {
	madt_entry_header_t hdr;
	uint8_t  acpi_processor_id;
	uint8_t  apic_id;
	uint32_t flags;
} __attribute__((packed)) madt_lapic_t;

typedef struct {
	madt_entry_header_t hdr;
	uint8_t  ioapic_id;
	uint8_t  reserved;
	uint32_t ioapic_address;
	uint32_t gsi_base;
} __attribute__((packed)) madt_ioapic_t;

typedef struct {
	madt_entry_header_t hdr;
	uint8_t  bus;
	uint8_t  source;
	uint32_t gsi;
	uint16_t flags;
} __attribute__((packed)) madt_iso_t;

/* ── State ──────────────────────────────────────────────────────── */

static acpi_madt_info_t madt_info;
static const rsdt_t*    cached_rsdt = 0;
static uint32_t         cached_entry_count = 0;

/* ── FADT / power management ────────────────────────────────────── */

typedef struct {
	acpi_sdt_header_t header;
	uint32_t firmware_ctrl;
	uint32_t dsdt;
	uint8_t  reserved1;
	uint8_t  preferred_pm_profile;
	uint16_t sci_interrupt;
	uint32_t smi_command_port;
	uint8_t  acpi_enable;
	uint8_t  acpi_disable;
	uint8_t  s4bios_req;
	uint8_t  pstate_control;
	uint32_t pm1a_event_block;
	uint32_t pm1b_event_block;
	uint32_t pm1a_control_block;
	uint32_t pm1b_control_block;
	uint32_t pm2_control_block;
	uint32_t pm_timer_block;
	uint32_t gpe0_block;
	uint32_t gpe1_block;
	uint8_t  pm1_event_length;
	uint8_t  pm1_control_length;
	uint8_t  pm2_control_length;
	uint8_t  pm_timer_length;
	uint8_t  gpe0_length;
	uint8_t  gpe1_length;
	uint8_t  gpe1_base;
	uint8_t  cstate_control;
	uint16_t worst_c2_latency;
	uint16_t worst_c3_latency;
	uint16_t flush_size;
	uint16_t flush_stride;
	uint8_t  duty_offset;
	uint8_t  duty_width;
	uint8_t  day_alarm;
	uint8_t  month_alarm;
	uint8_t  century;
	uint16_t iapc_boot_arch;
	uint8_t  reserved2;
	uint32_t flags;
	/* Generic Address Structure for reset register (FADT rev >= 2) */
	uint8_t  reset_reg_space;
	uint8_t  reset_reg_bit_width;
	uint8_t  reset_reg_bit_offset;
	uint8_t  reset_reg_access_size;
	uint64_t reset_reg_address;
	uint8_t  reset_value;
} __attribute__((packed)) fadt_t;

static int      fadt_available = 0;
static uint16_t pm1a_ctrl_port = 0;
static uint16_t pm1b_ctrl_port = 0;
static uint16_t slp_typa = 0;
static uint16_t slp_typb = 0;
/* FADT reboot info */
static int      fadt_has_reset = 0;
static uint8_t  fadt_reset_space = 0;
static uint64_t fadt_reset_address = 0;
static uint8_t  fadt_reset_value = 0;

/* Forward declarations */
static void parse_fadt(void);

/* ── Helpers ────────────────────────────────────────────────────── */

static int mem_compare(const void* a, const void* b, unsigned int n) {
	const uint8_t* pa = (const uint8_t*)a;
	const uint8_t* pb = (const uint8_t*)b;
	for (unsigned int i = 0; i < n; i++) {
		if (pa[i] != pb[i]) return 1;
	}
	return 0;
}

static int checksum_valid(const void* ptr, unsigned int length) {
	const uint8_t* p = (const uint8_t*)ptr;
	uint8_t sum = 0;
	for (unsigned int i = 0; i < length; i++) {
		sum += p[i];
	}
	return sum == 0;
}

/* ── RSDP search ────────────────────────────────────────────────── */

static const rsdp_t* find_rsdp_in(uint32_t start, uint32_t length) {
	const uint8_t* base = (const uint8_t*)(uintptr_t)start;
	for (uint32_t offset = 0; offset + 20 <= length; offset += 16) {
		const rsdp_t* candidate = (const rsdp_t*)(base + offset);
		if (mem_compare(candidate->signature, "RSD PTR ", 8) == 0) {
			if (checksum_valid(candidate, 20)) {
				return candidate;
			}
		}
	}
	return 0;
}

static const rsdp_t* find_rsdp(void) {
	const rsdp_t* rsdp;

	/* Search EBDA (Extended BIOS Data Area) */
	uint16_t ebda_seg = *(const uint16_t*)(uintptr_t)0x040E;
	uint32_t ebda_addr = (uint32_t)ebda_seg << 4;
	if (ebda_addr >= 0x80000 && ebda_addr < 0xA0000) {
		rsdp = find_rsdp_in(ebda_addr, 1024);
		if (rsdp) return rsdp;
	}

	/* Search BIOS ROM area */
	rsdp = find_rsdp_in(0xE0000, 0x20000);
	return rsdp;
}

/* ── MADT parsing ───────────────────────────────────────────────── */

static void parse_madt(const madt_header_t* madt) {
	uint32_t total_length = madt->header.length;
	uint32_t offset = sizeof(madt_header_t);
	const uint8_t* base = (const uint8_t*)madt;

	madt_info.lapic_address = madt->lapic_address;

	while (offset + 2 <= total_length) {
		const madt_entry_header_t* entry =
			(const madt_entry_header_t*)(base + offset);

		if (entry->length < 2 || offset + entry->length > total_length) {
			break;
		}

		switch (entry->type) {
		case ACPI_MADT_TYPE_LAPIC: {
			const madt_lapic_t* la = (const madt_lapic_t*)entry;
			if (madt_info.lapic_count < ACPI_MAX_LAPICS) {
				acpi_lapic_entry_t* e =
					&madt_info.lapics[madt_info.lapic_count++];
				e->acpi_id  = la->acpi_processor_id;
				e->lapic_id = la->apic_id;
				e->flags    = la->flags;
			}
			break;
		}
		case ACPI_MADT_TYPE_IOAPIC: {
			const madt_ioapic_t* io = (const madt_ioapic_t*)entry;
			if (madt_info.ioapic_count < ACPI_MAX_IOAPICS) {
				acpi_ioapic_entry_t* e =
					&madt_info.ioapics[madt_info.ioapic_count++];
				e->ioapic_id = io->ioapic_id;
				e->ioapic_address = io->ioapic_address;
				e->gsi_base = io->gsi_base;
			}
			break;
		}
		case ACPI_MADT_TYPE_ISO: {
			const madt_iso_t* iso = (const madt_iso_t*)entry;
			if (madt_info.iso_count < ACPI_MAX_ISO) {
				acpi_iso_entry_t* e =
					&madt_info.isos[madt_info.iso_count++];
				e->bus = iso->bus;
				e->source = iso->source;
				e->gsi = iso->gsi;
				e->flags = iso->flags;
			}
			break;
		}
		default:
			break;
		}

		offset += entry->length;
	}
}

/* ── Public API ─────────────────────────────────────────────────── */

void acpi_init(void) {
	const rsdp_t* rsdp;
	const rsdt_t* rsdt;
	uint32_t entry_count;

	madt_info.found = 0;
	madt_info.lapic_address = 0;
	madt_info.lapic_count = 0;
	madt_info.ioapic_count = 0;
	madt_info.iso_count = 0;

	rsdp = find_rsdp();
	if (!rsdp) {
		klog_write(KLOG_LEVEL_WARN, "acpi", "RSDP no encontrado");
		return;
	}

	rsdt = (const rsdt_t*)(uintptr_t)rsdp->rsdt_address;
	if (mem_compare(rsdt->header.signature, "RSDT", 4) != 0) {
		klog_write(KLOG_LEVEL_WARN, "acpi", "RSDT signature invalida");
		return;
	}

	if (!checksum_valid(rsdt, rsdt->header.length)) {
		klog_write(KLOG_LEVEL_WARN, "acpi", "RSDT checksum invalido");
		return;
	}

	entry_count = (rsdt->header.length - sizeof(acpi_sdt_header_t))
		/ sizeof(uint32_t);

	/* Cache for acpi_find_table() */
	cached_rsdt = rsdt;
	cached_entry_count = entry_count;

	for (uint32_t i = 0; i < entry_count; i++) {
		const acpi_sdt_header_t* hdr =
			(const acpi_sdt_header_t*)(uintptr_t)rsdt->entries[i];

		if (mem_compare(hdr->signature, "APIC", 4) == 0 &&
		    checksum_valid(hdr, hdr->length)) {
			parse_madt((const madt_header_t*)hdr);
			madt_info.found = 1;
			klog_write(KLOG_LEVEL_INFO, "acpi", "MADT encontrada");
			break;
		}
	}

	if (!madt_info.found) {
		klog_write(KLOG_LEVEL_WARN, "acpi", "MADT no encontrada en RSDT");
	}

	/* Parse FADT for power management (shutdown/reboot) */
	parse_fadt();
}

const acpi_madt_info_t* acpi_get_madt_info(void) {
	return &madt_info;
}

const void* acpi_find_table(const char* signature) {
	if (!cached_rsdt || !signature) {
		return 0;
	}

	for (uint32_t i = 0; i < cached_entry_count; i++) {
		const acpi_sdt_header_t* hdr =
			(const acpi_sdt_header_t*)(uintptr_t)cached_rsdt->entries[i];

		if (mem_compare(hdr->signature, signature, 4) == 0 &&
		    checksum_valid(hdr, hdr->length)) {
			return hdr;
		}
	}

	return 0;
}

/* ── FADT / DSDT S5 parsing ─────────────────────────────────────── */

static void parse_fadt(void) {
	const fadt_t* fadt = (const fadt_t*)acpi_find_table("FACP");
	if (!fadt) {
		klog_write(KLOG_LEVEL_WARN, "acpi", "FADT no encontrada");
		return;
	}

	pm1a_ctrl_port = (uint16_t)fadt->pm1a_control_block;
	pm1b_ctrl_port = (uint16_t)fadt->pm1b_control_block;

	/* Extract S5 sleep type from DSDT \_S5 object */
	if (fadt->dsdt) {
		const uint8_t* dsdt = (const uint8_t*)(uintptr_t)fadt->dsdt;
		const acpi_sdt_header_t* dh = (const acpi_sdt_header_t*)dsdt;
		uint32_t dsdt_len = dh->length;
		/* Search for the AML name object "_S5_" (bytes: 08 5F 53 35 5F) */
		for (uint32_t i = sizeof(acpi_sdt_header_t); i + 8 < dsdt_len; i++) {
			if (dsdt[i] == '_' && dsdt[i+1] == 'S' &&
			    dsdt[i+2] == '5' && dsdt[i+3] == '_') {
				/* Skip past _S5_ name + package op + length */
				uint32_t j = i + 4;
				if (j < dsdt_len && dsdt[j] == 0x12) { /* PackageOp */
					j++;
					/* Skip package length (1-4 byte encoding) */
					if (dsdt[j] & 0xC0) {
						j += (dsdt[j] >> 6) + 1;
					} else {
						j++;
					}
					j++; /* package element count */
					/* Read SLP_TYPa */
					if (j < dsdt_len && dsdt[j] == 0x0A) { /* BytePrefix */
						j++;
						slp_typa = dsdt[j];
						j++;
					} else if (j < dsdt_len) {
						slp_typa = dsdt[j];
						j++;
					}
					/* Read SLP_TYPb */
					if (j < dsdt_len && dsdt[j] == 0x0A) {
						j++;
						slp_typb = dsdt[j];
					} else if (j < dsdt_len) {
						slp_typb = dsdt[j];
					}
				}
				break;
			}
		}
	}

	/* Check for reset register (FADT revision >= 2, bit 10 of flags) */
	if (fadt->header.revision >= 2 && (fadt->flags & (1U << 10))) {
		fadt_has_reset = 1;
		fadt_reset_space = fadt->reset_reg_space;
		fadt_reset_address = fadt->reset_reg_address;
		fadt_reset_value = fadt->reset_value;
	}

	fadt_available = 1;
	klog_write(KLOG_LEVEL_INFO, "acpi", "FADT: power management disponible");
}

/* ── Power management API ───────────────────────────────────────── */

int acpi_power_available(void) {
	return fadt_available;
}

int acpi_shutdown(void) {
	if (!fadt_available || !pm1a_ctrl_port) {
		return -1;
	}

	/* Disable interrupts */
	__asm__ volatile ("cli");

	/* Write SLP_TYPa | SLP_EN to PM1a control register */
	outw(pm1a_ctrl_port, (slp_typa << 10) | (1 << 13));

	/* If PM1b exists, write there too */
	if (pm1b_ctrl_port) {
		outw(pm1b_ctrl_port, (slp_typb << 10) | (1 << 13));
	}

	/* If we get here, shutdown failed.  Halt. */
	for (;;) __asm__ volatile ("hlt");
	return -1;
}

int acpi_reboot(void) {
	/* Method 1: FADT reset register */
	if (fadt_has_reset && fadt_reset_address) {
		__asm__ volatile ("cli");
		if (fadt_reset_space == 1) {
			/* System I/O space */
			outb((uint16_t)fadt_reset_address, fadt_reset_value);
		}
		/* Wait a moment */
		for (volatile int i = 0; i < 100000; i++) {}
	}

	/* Method 2: PS/2 keyboard controller reset (0x64 port) */
	__asm__ volatile ("cli");
	outb(0x64, 0xFE);

	/* Method 3: triple fault */
	for (volatile int i = 0; i < 100000; i++) {}
	__asm__ volatile (
		"lidt %0\n\t"
		"int $3"
		: : "m"(*(uint16_t[]){0, 0, 0}) : "memory"
	);

	for (;;) __asm__ volatile ("hlt");
	return -1;
}
