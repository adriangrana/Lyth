#ifndef SMP_H
#define SMP_H

#include <stdint.h>

#define SMP_MAX_CPUS 16

/* Initialise SMP: discover CPUs via ACPI MADT and boot APs.
   Must be called after acpi_init() and apic_init(). */
void smp_init(void);

/* Number of online CPUs (BSP + APs that responded). */
int  smp_cpu_count(void);

/* LAPIC ID of the BSP. */
uint8_t smp_bsp_id(void);

#endif
