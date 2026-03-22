#ifndef PCI_H
#define PCI_H

#include <stdint.h>

#define PCI_CONFIG_ADDR  0x0CF8
#define PCI_CONFIG_DATA  0x0CFC

#define PCI_MAX_DEVICES  32

typedef struct {
	uint8_t  bus;
	uint8_t  slot;
	uint8_t  func;
	uint16_t vendor_id;
	uint16_t device_id;
	uint8_t  class_code;
	uint8_t  subclass;
	uint8_t  prog_if;
	uint8_t  header_type;
	uint8_t  irq_line;
	uint32_t bar[6];
} pci_device_t;

void     pci_init(void);
int      pci_device_count(void);
const pci_device_t* pci_get_device(int index);
const pci_device_t* pci_find_device(uint16_t vendor, uint16_t device);
const pci_device_t* pci_find_class(uint8_t class_code, uint8_t subclass);
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_enable_bus_mastering(const pci_device_t* dev);

#endif
