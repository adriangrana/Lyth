#ifndef E1000_H
#define E1000_H

#include <stdint.h>

/* PCI identifiers */
#define E1000_VENDOR_ID   0x8086
#define E1000_DEVICE_ID   0x100E  /* 82540EM (QEMU default) */
#define E1000_DEVICE_ID2  0x100F  /* 82545EM */
#define E1000_DEVICE_ID3  0x10D3  /* 82574L  */
#define E1000_DEVICE_ID4  0x153A  /* I217-LM */

/* Register offsets (MMIO) */
#define E1000_CTRL    0x0000
#define E1000_STATUS  0x0008
#define E1000_EERD    0x0014
#define E1000_ICR     0x00C0
#define E1000_ITR     0x00C4
#define E1000_ICS     0x00C8
#define E1000_IMS     0x00D0
#define E1000_IMC     0x00D8
#define E1000_RCTL    0x0100
#define E1000_TCTL    0x0400
#define E1000_TIPG    0x0410
#define E1000_RDBAL   0x2800
#define E1000_RDBAH   0x2804
#define E1000_RDLEN   0x2808
#define E1000_RDH     0x2810
#define E1000_RDT     0x2818
#define E1000_TDBAL   0x3800
#define E1000_TDBAH   0x3804
#define E1000_TDLEN   0x3808
#define E1000_TDH     0x3810
#define E1000_TDT     0x3818
#define E1000_MTA     0x5200
#define E1000_RAL     0x5400
#define E1000_RAH     0x5404

/* CTRL bits */
#define E1000_CTRL_FD     (1U << 0)
#define E1000_CTRL_ASDE   (1U << 5)
#define E1000_CTRL_SLU    (1U << 6)
#define E1000_CTRL_RST    (1U << 26)

/* RCTL bits */
#define E1000_RCTL_EN     (1U << 1)
#define E1000_RCTL_SBP    (1U << 2)
#define E1000_RCTL_UPE    (1U << 3)
#define E1000_RCTL_MPE    (1U << 4)
#define E1000_RCTL_BAM    (1U << 15)
#define E1000_RCTL_SECRC  (1U << 26)
#define E1000_RCTL_BSIZE_2048  0    /* buf size in RCTL.BSIZE */

/* TCTL bits */
#define E1000_TCTL_EN     (1U << 1)
#define E1000_TCTL_PSP    (1U << 3)

/* ICR / IMS interrupt bits */
#define E1000_ICR_TXDW    (1U << 0)
#define E1000_ICR_TXQE    (1U << 1)
#define E1000_ICR_LSC     (1U << 2)
#define E1000_ICR_RXSEQ   (1U << 3)
#define E1000_ICR_RXDMT0  (1U << 4)
#define E1000_ICR_RXO     (1U << 6)
#define E1000_ICR_RXT0    (1U << 7)

/* Descriptor ring sizes */
#define E1000_NUM_RX_DESC 32
#define E1000_NUM_TX_DESC 8
#define E1000_RX_BUF_SIZE 2048

/* RX descriptor (legacy) */
typedef struct __attribute__((packed)) {
	uint64_t addr;
	uint16_t length;
	uint16_t checksum;
	uint8_t  status;
	uint8_t  errors;
	uint16_t special;
} e1000_rx_desc_t;

/* TX descriptor (legacy) */
typedef struct __attribute__((packed)) {
	uint64_t addr;
	uint16_t length;
	uint8_t  cso;
	uint8_t  cmd;
	uint8_t  status;
	uint8_t  css;
	uint16_t special;
} e1000_tx_desc_t;

/* RX descriptor status bits */
#define E1000_RXD_STAT_DD   (1U << 0)
#define E1000_RXD_STAT_EOP  (1U << 1)

/* TX descriptor CMD bits */
#define E1000_TXD_CMD_EOP   (1U << 0)
#define E1000_TXD_CMD_IFCS  (1U << 1)
#define E1000_TXD_CMD_RS    (1U << 3)

/* TX descriptor status bits */
#define E1000_TXD_STAT_DD   (1U << 0)

int  e1000_init(void);
int  e1000_link_up(void);
int  e1000_send(const uint8_t* data, uint16_t len);
void e1000_get_mac(uint8_t mac[6]);
void e1000_irq_handler(void);
void e1000_poll_rx(void);

#endif
