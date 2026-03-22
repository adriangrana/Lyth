#ifndef ACPI_H
#define ACPI_H

#include <stdint.h>

#define ACPI_MADT_TYPE_LAPIC       0
#define ACPI_MADT_TYPE_IOAPIC      1
#define ACPI_MADT_TYPE_ISO         2
#define ACPI_MADT_TYPE_LAPIC_NMI   4

#define ACPI_MAX_IOAPICS           4
#define ACPI_MAX_ISO               24
#define ACPI_MAX_LAPICS            16

typedef struct {
	uint8_t  acpi_id;
	uint8_t  lapic_id;
	uint32_t flags;          /* bit 0: processor enabled */
} acpi_lapic_entry_t;

typedef struct {
	uint32_t ioapic_address;
	uint32_t ioapic_id;
	uint32_t gsi_base;
} acpi_ioapic_entry_t;

typedef struct {
	uint8_t  bus;
	uint8_t  source;
	uint32_t gsi;
	uint16_t flags;
} acpi_iso_entry_t;

typedef struct {
	int      found;
	uint32_t lapic_address;
	int      lapic_count;
	acpi_lapic_entry_t  lapics[ACPI_MAX_LAPICS];
	int      ioapic_count;
	acpi_ioapic_entry_t ioapics[ACPI_MAX_IOAPICS];
	int      iso_count;
	acpi_iso_entry_t    isos[ACPI_MAX_ISO];
} acpi_madt_info_t;

void acpi_init(void);
const acpi_madt_info_t* acpi_get_madt_info(void);

#endif
