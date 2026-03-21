#ifndef ATA_H
#define ATA_H

#include <stdint.h>

/* ---- Geometry / limits ---- */
#define ATA_SECTOR_SIZE  512    /* bytes per logical sector          */
#define ATA_DRIVE_MASTER 0      /* primary master (drive 0)          */
#define ATA_DRIVE_SLAVE  1      /* primary slave  (drive 1)          */

/* ---- Drive info filled by ata_init() ---- */
typedef struct {
    int      present;           /* 1 if the drive responded to IDENTIFY */
    uint32_t sectors;           /* total LBA28 sector count (max ~128 GB) */
    char     model[41];         /* null-terminated model string from IDENTIFY */
} ata_drive_info_t;

/* Initialise the ATA subsystem.
   Probes the primary bus (master + slave) using the IDENTIFY command.
   Must be called after interrupts are initialised (so the PIC mask is set). */
void ata_init(void);

/* Returns 1 if the given drive (ATA_DRIVE_MASTER/SLAVE) is present. */
int  ata_is_present(int drive);

/* Copy drive information into *info.  Returns 0 on success, -1 if absent. */
int  ata_get_info(int drive, ata_drive_info_t* info);

/* Read 'count' sectors (each 512 bytes) beginning at 'lba' from 'drive'.
   'buf' must be at least count * ATA_SECTOR_SIZE bytes.
   Returns the number of sectors successfully read, or -1 on error. */
int  ata_read (int drive, uint32_t lba, uint8_t count, uint8_t* buf);

/* Write 'count' sectors beginning at 'lba' to 'drive'.
   Returns the number of sectors successfully written, or -1 on error. */
int  ata_write(int drive, uint32_t lba, uint8_t count, const uint8_t* buf);

/* Called from the IRQ14 interrupt handler. Sends EOI and returns esp. */
unsigned int ata_irq14_handler(unsigned int current_esp);

#endif
