#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "physmem.h"
#include "heap.h"
#include "klog.h"
#include "netif.h"
#include "string.h"

/* ── MMIO helpers ───────────────────────────────────────────────── */

static volatile uint32_t* mmio_base;

static void e1000_write(uint32_t reg, uint32_t val) {
	mmio_base[reg / 4] = val;
}

static uint32_t e1000_read(uint32_t reg) {
	return mmio_base[reg / 4];
}

/* ── Descriptors & buffers ──────────────────────────────────────── */

static e1000_rx_desc_t* rx_descs;
static e1000_tx_desc_t* tx_descs;
static uint32_t rx_descs_phys;
static uint32_t tx_descs_phys;
static uint8_t* rx_bufs[E1000_NUM_RX_DESC];
static uint32_t rx_bufs_phys[E1000_NUM_RX_DESC];
static uint16_t rx_cur;
static uint16_t tx_cur;

static uint8_t mac_addr[6];
static netif_t* e1000_netif;
static int e1000_present;

/* ── MAC address read ───────────────────────────────────────────── */

static void e1000_read_mac_eeprom(void) {
	/* Try EEPROM read (EERD) */
	e1000_write(E1000_EERD, 0x01);  /* start read, address 0 */
	uint32_t val;
	int tries = 1000;
	do {
		val = e1000_read(E1000_EERD);
	} while (!(val & (1U << 4)) && --tries);

	if (val & (1U << 4)) {
		/* EEPROM present */
		uint32_t tmp;
		e1000_write(E1000_EERD, (0 << 8) | 1);
		while (!((tmp = e1000_read(E1000_EERD)) & (1U << 4)));
		mac_addr[0] = (uint8_t)(tmp >> 16);
		mac_addr[1] = (uint8_t)(tmp >> 24);

		e1000_write(E1000_EERD, (1 << 8) | 1);
		while (!((tmp = e1000_read(E1000_EERD)) & (1U << 4)));
		mac_addr[2] = (uint8_t)(tmp >> 16);
		mac_addr[3] = (uint8_t)(tmp >> 24);

		e1000_write(E1000_EERD, (2 << 8) | 1);
		while (!((tmp = e1000_read(E1000_EERD)) & (1U << 4)));
		mac_addr[4] = (uint8_t)(tmp >> 16);
		mac_addr[5] = (uint8_t)(tmp >> 24);
	} else {
		/* Read from RAL/RAH registers (QEMU fallback) */
		uint32_t ral = e1000_read(E1000_RAL);
		uint32_t rah = e1000_read(E1000_RAH);
		mac_addr[0] = (uint8_t)(ral);
		mac_addr[1] = (uint8_t)(ral >> 8);
		mac_addr[2] = (uint8_t)(ral >> 16);
		mac_addr[3] = (uint8_t)(ral >> 24);
		mac_addr[4] = (uint8_t)(rah);
		mac_addr[5] = (uint8_t)(rah >> 8);
	}
}

/* ── RX init ────────────────────────────────────────────────────── */

static void e1000_rx_init(void) {
	/* Allocate descriptor ring (must be 16-byte aligned) */
	uint32_t desc_sz = (uint32_t)(sizeof(e1000_rx_desc_t) * E1000_NUM_RX_DESC);
	rx_descs_phys = physmem_alloc_region(desc_sz, 16);
	paging_map_mmio(rx_descs_phys);
	rx_descs = (e1000_rx_desc_t*)(uintptr_t)rx_descs_phys;

	for (int i = 0; i < E1000_NUM_RX_DESC; i++) {
		rx_bufs_phys[i] = physmem_alloc_region(E1000_RX_BUF_SIZE, 16);
		paging_map_mmio(rx_bufs_phys[i]);
		rx_bufs[i] = (uint8_t*)(uintptr_t)rx_bufs_phys[i];
		rx_descs[i].addr   = (uint64_t)rx_bufs_phys[i];
		rx_descs[i].status = 0;
	}

	e1000_write(E1000_RDBAL, rx_descs_phys);
	e1000_write(E1000_RDBAH, 0);
	e1000_write(E1000_RDLEN, desc_sz);
	e1000_write(E1000_RDH, 0);
	e1000_write(E1000_RDT, E1000_NUM_RX_DESC - 1);

	/* Enable receiver: accept broadcast, strip CRC */
	e1000_write(E1000_RCTL,
		E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC | E1000_RCTL_BSIZE_2048);

	rx_cur = 0;
}

/* ── TX init ────────────────────────────────────────────────────── */

static void e1000_tx_init(void) {
	uint32_t desc_sz = (uint32_t)(sizeof(e1000_tx_desc_t) * E1000_NUM_TX_DESC);
	tx_descs_phys = physmem_alloc_region(desc_sz, 16);
	paging_map_mmio(tx_descs_phys);
	tx_descs = (e1000_tx_desc_t*)(uintptr_t)tx_descs_phys;

	for (int i = 0; i < E1000_NUM_TX_DESC; i++) {
		tx_descs[i].addr   = 0;
		tx_descs[i].cmd    = 0;
		tx_descs[i].status = E1000_TXD_STAT_DD; /* mark as done */
	}

	e1000_write(E1000_TDBAL, tx_descs_phys);
	e1000_write(E1000_TDBAH, 0);
	e1000_write(E1000_TDLEN, desc_sz);
	e1000_write(E1000_TDH, 0);
	e1000_write(E1000_TDT, 0);

	/* Enable transmitter */
	e1000_write(E1000_TCTL,
		E1000_TCTL_EN | E1000_TCTL_PSP | (15U << 4) /* CT */ | (64U << 12) /* COLD */);

	/* Inter-packet gap */
	e1000_write(E1000_TIPG, 10 | (8 << 10) | (6 << 20));

	tx_cur = 0;
}

/* ── Send wrapper for netif ─────────────────────────────────────── */

static int e1000_netif_send(netif_t* iface, const uint8_t* data, uint16_t len) {
	(void)iface;
	return e1000_send(data, len);
}

/* ── Public API ─────────────────────────────────────────────────── */

int e1000_init(void) {
	const pci_device_t* dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID);
	if (!dev)
		dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID2);
	if (!dev)
		dev = pci_find_device(E1000_VENDOR_ID, E1000_DEVICE_ID3);
	if (!dev)
		dev = pci_find_class(0x02, 0x00); /* network controller, ethernet */

	if (!dev) {
		klog_write(KLOG_LEVEL_WARN, "e1000", "NIC no encontrada");
		e1000_present = 0;
		return -1;
	}

	e1000_present = 1;

	/* Enable bus mastering for DMA */
	pci_enable_bus_mastering(dev);

	/* Map MMIO BAR0 */
	uint32_t bar0 = dev->bar[0] & ~0xFU;
	paging_map_mmio(bar0);
	mmio_base = (volatile uint32_t*)(uintptr_t)bar0;

	/* Reset */
	e1000_write(E1000_CTRL, e1000_read(E1000_CTRL) | E1000_CTRL_RST);
	for (volatile int i = 0; i < 100000; i++);

	/* Set link up */
	e1000_write(E1000_CTRL, E1000_CTRL_SLU | E1000_CTRL_ASDE);

	/* Clear multicast table */
	for (int i = 0; i < 128; i++)
		e1000_write(E1000_MTA + (uint32_t)(i * 4), 0);

	e1000_read_mac_eeprom();

	e1000_rx_init();
	e1000_tx_init();

	/* Enable interrupts: RX timer, link change, RX overrun */
	e1000_write(E1000_IMS,
		E1000_ICR_RXT0 | E1000_ICR_LSC | E1000_ICR_RXO | E1000_ICR_RXDMT0);

	/* Register network interface */
	e1000_netif = netif_register("eth0", mac_addr, e1000_netif_send);

	klog_write(KLOG_LEVEL_INFO, "e1000", "NIC inicializada");
	return 0;
}

int e1000_link_up(void) {
	if (!e1000_present || !mmio_base) return 0;
	return (e1000_read(E1000_STATUS) & (1U << 1)) ? 1 : 0;
}

int e1000_send(const uint8_t* data, uint16_t len) {
	if (!e1000_present || len == 0)
		return -1;

	/* Wait for previous TX to complete */
	while (!(tx_descs[tx_cur].status & E1000_TXD_STAT_DD));

	/* Copy data into a DMA-accessible buffer */
	uint32_t buf_phys = physmem_alloc_region(len, 16);
	if (!buf_phys) return -1;
	paging_map_mmio(buf_phys);
	memcpy((void*)(uintptr_t)buf_phys, data, len);

	tx_descs[tx_cur].addr   = (uint64_t)buf_phys;
	tx_descs[tx_cur].length = len;
	tx_descs[tx_cur].cmd    = E1000_TXD_CMD_EOP | E1000_TXD_CMD_IFCS | E1000_TXD_CMD_RS;
	tx_descs[tx_cur].status = 0;

	uint16_t old_cur = tx_cur;
	tx_cur = (tx_cur + 1) % E1000_NUM_TX_DESC;
	e1000_write(E1000_TDT, tx_cur);

	/* Wait for send to complete, then free buffer */
	while (!(tx_descs[old_cur].status & E1000_TXD_STAT_DD));
	physmem_free_region(buf_phys, len);

	return 0;
}

void e1000_get_mac(uint8_t mac[6]) {
	memcpy(mac, mac_addr, 6);
}

void e1000_irq_handler(void) {
	if (!e1000_present)
		return;

	uint32_t icr = e1000_read(E1000_ICR);
	(void)icr; /* reading ICR clears the interrupt */

	e1000_poll_rx();
}

void e1000_poll_rx(void) {
	if (!e1000_present)
		return;

	/* Process all pending RX descriptors */
	while (rx_descs[rx_cur].status & E1000_RXD_STAT_DD) {
		uint16_t pkt_len = rx_descs[rx_cur].length;
		uint8_t* pkt_data = rx_bufs[rx_cur];

		if ((rx_descs[rx_cur].status & E1000_RXD_STAT_EOP) && pkt_len > 0) {
			/* Deliver to network stack */
			if (e1000_netif)
				netif_rx(e1000_netif, pkt_data, pkt_len);
		}

		/* Reset descriptor */
		rx_descs[rx_cur].status = 0;
		uint16_t old_cur = rx_cur;
		rx_cur = (rx_cur + 1) % E1000_NUM_RX_DESC;
		e1000_write(E1000_RDT, old_cur);
	}
}
