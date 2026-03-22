#include "acpi.h"
#include "klog.h"

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

	for (uint32_t i = 0; i < entry_count; i++) {
		const acpi_sdt_header_t* hdr =
			(const acpi_sdt_header_t*)(uintptr_t)rsdt->entries[i];

		if (mem_compare(hdr->signature, "APIC", 4) == 0 &&
		    checksum_valid(hdr, hdr->length)) {
			parse_madt((const madt_header_t*)hdr);
			madt_info.found = 1;
			klog_write(KLOG_LEVEL_INFO, "acpi", "MADT encontrada");
			return;
		}
	}

	klog_write(KLOG_LEVEL_WARN, "acpi", "MADT no encontrada en RSDT");
}

const acpi_madt_info_t* acpi_get_madt_info(void) {
	return &madt_info;
}
