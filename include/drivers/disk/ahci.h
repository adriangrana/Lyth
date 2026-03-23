#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>

/* Maximum AHCI ports and registered drives */
#define AHCI_MAX_PORTS 32
#define AHCI_MAX_DRIVES 8

/* AHCI FIS types */
#define FIS_TYPE_REG_H2D    0x27
#define FIS_TYPE_REG_D2H    0x34
#define FIS_TYPE_DMA_SETUP  0x41
#define FIS_TYPE_DATA       0x46
#define FIS_TYPE_PIO_SETUP  0x5F

/* ATA commands */
#define ATA_CMD_IDENTIFY     0xEC
#define ATA_CMD_READ_DMA_EX  0x25
#define ATA_CMD_WRITE_DMA_EX 0x35
#define ATA_CMD_FLUSH_CACHE  0xE7

/* Port signature values */
#define SATA_SIG_ATA    0x00000101
#define SATA_SIG_ATAPI  0xEB140101
#define SATA_SIG_SEMB   0xC33C0101
#define SATA_SIG_PM     0x96690101

/* ── AHCI HBA Memory Registers ─────────────────────────────────── */

typedef volatile struct {
    uint32_t clb;           /* Command list base address (low) */
    uint32_t clbu;          /* Command list base address (high) */
    uint32_t fb;            /* FIS base address (low) */
    uint32_t fbu;           /* FIS base address (high) */
    uint32_t is;            /* Interrupt status */
    uint32_t ie;            /* Interrupt enable */
    uint32_t cmd;           /* Command and status */
    uint32_t rsv0;
    uint32_t tfd;           /* Task file data */
    uint32_t sig;           /* Signature */
    uint32_t ssts;          /* SATA status (SCR0) */
    uint32_t sctl;          /* SATA control (SCR2) */
    uint32_t serr;          /* SATA error (SCR1) */
    uint32_t sact;          /* SATA active (SCR3) */
    uint32_t ci;            /* Command issue */
    uint32_t sntf;          /* SATA notification */
    uint32_t fbs;           /* FIS-based switching control */
    uint32_t rsv1[11];
    uint32_t vendor[4];
} ahci_port_regs_t;

typedef volatile struct {
    uint32_t cap;           /* Host capabilities */
    uint32_t ghc;           /* Global host control */
    uint32_t is;            /* Interrupt status */
    uint32_t pi;            /* Ports implemented */
    uint32_t vs;            /* Version */
    uint32_t ccc_ctl;
    uint32_t ccc_ports;
    uint32_t em_loc;
    uint32_t em_ctl;
    uint32_t cap2;          /* Extended capabilities */
    uint32_t bohc;          /* BIOS/OS handoff control */
    uint8_t  rsv[0x74];
    uint8_t  vendor[0x60];
    ahci_port_regs_t ports[32];
} ahci_hba_mem_t;

/* ── Command structures ─────────────────────────────────────────── */

typedef struct {
    uint8_t  cfis_length:5;
    uint8_t  atapi:1;
    uint8_t  write:1;
    uint8_t  prefetchable:1;
    uint8_t  reset:1;
    uint8_t  bist:1;
    uint8_t  clear_busy:1;
    uint8_t  rsv0:1;
    uint8_t  pmp:4;
    uint16_t prdtl;         /* Physical region descriptor table length */
    volatile uint32_t prdbc; /* PRD byte count transferred */
    uint32_t ctba;          /* Command table descriptor base address (low) */
    uint32_t ctbau;         /* Command table descriptor base address (high) */
    uint32_t rsv1[4];
} __attribute__((packed)) ahci_cmd_header_t;

typedef struct {
    uint32_t dba;           /* Data base address (low) */
    uint32_t dbau;          /* Data base address (high) */
    uint32_t rsv0;
    uint32_t dbc:22;        /* Byte count (0-based, max 4MB) */
    uint32_t rsv1:9;
    uint32_t i:1;           /* Interrupt on completion */
} __attribute__((packed)) ahci_prdt_entry_t;

typedef struct {
    uint8_t  cfis[64];      /* Command FIS */
    uint8_t  acmd[16];      /* ATAPI command */
    uint8_t  rsv[48];
    ahci_prdt_entry_t prdt[8]; /* Up to 8 PRD entries (32KB max per command) */
} __attribute__((packed)) ahci_cmd_table_t;

/* FIS Register H2D */
typedef struct {
    uint8_t  fis_type;
    uint8_t  pmport:4;
    uint8_t  rsv0:3;
    uint8_t  c:1;           /* 1 = Command, 0 = Control */
    uint8_t  command;
    uint8_t  featurel;
    uint8_t  lba0;
    uint8_t  lba1;
    uint8_t  lba2;
    uint8_t  device;
    uint8_t  lba3;
    uint8_t  lba4;
    uint8_t  lba5;
    uint8_t  featureh;
    uint16_t count;
    uint8_t  icc;
    uint8_t  control;
    uint32_t rsv1;
} __attribute__((packed)) fis_reg_h2d_t;

/* ── AHCI drive info ────────────────────────────────────────────── */

typedef struct {
    int       port;         /* AHCI port number (0-31) */
    uint32_t  sector_count; /* Total sectors (LBA48 capped to 32-bit) */
    char      model[41];    /* Drive model string */
} ahci_drive_info_t;

/* ── Public API ─────────────────────────────────────────────────── */

void ahci_init(void);
int  ahci_is_available(void);
int  ahci_drive_count(void);
const ahci_drive_info_t* ahci_get_drive(int index);

/* Read/write 'count' 512-byte sectors starting at 'lba'.
   Returns 0 on success, -1 on error. */
int  ahci_read(int drive_index, uint32_t lba, uint32_t count, uint8_t* buf);
int  ahci_write(int drive_index, uint32_t lba, uint32_t count, const uint8_t* buf);

#endif
