#include "pci.h"
#include "klog.h"

/* ── I/O helpers ────────────────────────────────────────────────── */

static inline void outl(uint16_t port, uint32_t value) {
	__asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint32_t inl(uint16_t port) {
	uint32_t v;
	__asm__ volatile ("inl %1, %0" : "=a"(v) : "Nd"(port));
	return v;
}

static inline void outw(uint16_t port, uint16_t value) {
	__asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

/* ── PCI config space ───────────────────────────────────────────── */

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
	uint32_t address = (1U << 31)
		| ((uint32_t)bus << 16)
		| ((uint32_t)(slot & 0x1F) << 11)
		| ((uint32_t)(func & 0x07) << 8)
		| ((uint32_t)offset & 0xFC);
	outl(PCI_CONFIG_ADDR, address);
	return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
	uint32_t address = (1U << 31)
		| ((uint32_t)bus << 16)
		| ((uint32_t)(slot & 0x1F) << 11)
		| ((uint32_t)(func & 0x07) << 8)
		| ((uint32_t)offset & 0xFC);
	outl(PCI_CONFIG_ADDR, address);
	outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
	uint32_t val = pci_config_read32(bus, slot, func, offset & 0xFC);
	return (uint16_t)(val >> ((offset & 2) * 8));
}

/* ── Device table ───────────────────────────────────────────────── */

static pci_device_t devices[PCI_MAX_DEVICES];
static int device_count;

static void pci_scan_bus(uint8_t bus);

static void pci_scan_func(uint8_t bus, uint8_t slot, uint8_t func) {
	uint32_t reg0, reg2, reg3, regF;
	pci_device_t* d;

	reg0 = pci_config_read32(bus, slot, func, 0x00);
	if ((reg0 & 0xFFFF) == 0xFFFF)
		return;

	if (device_count >= PCI_MAX_DEVICES)
		return;

	d = &devices[device_count++];
	d->bus   = bus;
	d->slot  = slot;
	d->func  = func;
	d->vendor_id = (uint16_t)(reg0 & 0xFFFF);
	d->device_id = (uint16_t)(reg0 >> 16);

	reg2 = pci_config_read32(bus, slot, func, 0x08);
	d->class_code = (uint8_t)(reg2 >> 24);
	d->subclass   = (uint8_t)(reg2 >> 16);
	d->prog_if    = (uint8_t)(reg2 >> 8);

	reg3 = pci_config_read32(bus, slot, func, 0x0C);
	d->header_type = (uint8_t)(reg3 >> 16);

	regF = pci_config_read32(bus, slot, func, 0x3C);
	d->irq_line = (uint8_t)(regF & 0xFF);

	/* Read BARs (only for header type 0) */
	if ((d->header_type & 0x7F) == 0) {
		for (int i = 0; i < 6; i++)
			d->bar[i] = pci_config_read32(bus, slot, func, (uint8_t)(0x10 + i * 4));
	} else {
		for (int i = 0; i < 6; i++)
			d->bar[i] = 0;
	}

	/* If PCI-to-PCI bridge, scan secondary bus */
	if (d->class_code == 0x06 && d->subclass == 0x04) {
		uint32_t buses = pci_config_read32(bus, slot, func, 0x18);
		pci_scan_bus((uint8_t)(buses >> 8));
	}
}

static void pci_scan_slot(uint8_t bus, uint8_t slot) {
	uint32_t reg0;

	reg0 = pci_config_read32(bus, slot, 0, 0x00);
	if ((reg0 & 0xFFFF) == 0xFFFF)
		return;

	pci_scan_func(bus, slot, 0);

	/* Multi-function device? */
	uint32_t hdr = pci_config_read32(bus, slot, 0, 0x0C);
	if (hdr & (1U << 23)) {
		for (uint8_t func = 1; func < 8; func++)
			pci_scan_func(bus, slot, func);
	}
}

static void pci_scan_bus(uint8_t bus) {
	for (uint8_t slot = 0; slot < 32; slot++)
		pci_scan_slot(bus, slot);
}

void pci_init(void) {
	device_count = 0;

	/* Check if host bridge is multi-function */
	uint32_t hdr0 = pci_config_read32(0, 0, 0, 0x0C);
	if (hdr0 & (1U << 23)) {
		for (uint8_t func = 0; func < 8; func++) {
			if ((pci_config_read32(0, 0, func, 0x00) & 0xFFFF) != 0xFFFF)
				pci_scan_bus(func);
		}
	} else {
		pci_scan_bus(0);
	}

	klog_write(KLOG_LEVEL_INFO, "pci", "Bus PCI escaneado");
}

int pci_device_count(void) {
	return device_count;
}

const pci_device_t* pci_get_device(int index) {
	if (index < 0 || index >= device_count)
		return 0;
	return &devices[index];
}

const pci_device_t* pci_find_device(uint16_t vendor, uint16_t device) {
	for (int i = 0; i < device_count; i++) {
		if (devices[i].vendor_id == vendor && devices[i].device_id == device)
			return &devices[i];
	}
	return 0;
}

const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass) {
	for (int i = 0; i < device_count; i++) {
		if (devices[i].class_code == class_code && devices[i].subclass == subclass)
			return &devices[i];
	}
	return 0;
}

void pci_enable_bus_mastering(const pci_device_t* dev) {
	uint32_t cmd;
	if (!dev) return;
	cmd = pci_config_read32(dev->bus, dev->slot, dev->func, 0x04);
	cmd |= (1U << 2);   /* Bus Master Enable */
	pci_config_write32(dev->bus, dev->slot, dev->func, 0x04, cmd);
}
