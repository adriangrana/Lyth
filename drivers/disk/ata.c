#include "ata.h"

/* ============================================================
 *  ATA PIO driver  —  primary bus only, LBA28, polling
 *
 *  Port map (primary bus):
 *    0x1F0  Data register (16-bit R/W)
 *    0x1F1  Error (R) / Features (W)
 *    0x1F2  Sector Count
 *    0x1F3  LBA  [7:0]
 *    0x1F4  LBA  [15:8]
 *    0x1F5  LBA  [23:16]
 *    0x1F6  Drive/Head  (LBA mode: bit7=1,bit6=1, bit4=drive, bits3:0=LBA[27:24])
 *    0x1F7  Status (R) / Command (W)
 *    0x3F6  Alt Status / Device Control
 * ============================================================ */

#define ATA_BASE        0x1F0
#define ATA_CTRL        0x3F6

/* Register offsets from ATA_BASE */
#define ATA_REG_DATA     0x00
#define ATA_REG_ERROR    0x01
#define ATA_REG_FEATURES 0x01
#define ATA_REG_SECCOUNT 0x02
#define ATA_REG_LBA0     0x03
#define ATA_REG_LBA1     0x04
#define ATA_REG_LBA2     0x05
#define ATA_REG_DRIVE    0x06
#define ATA_REG_STATUS   0x07
#define ATA_REG_COMMAND  0x07

/* Status register bits */
#define ATA_SR_ERR  0x01   /* error occurred              */
#define ATA_SR_DRQ  0x08   /* data request — drive ready  */
#define ATA_SR_DF   0x20   /* drive fault                 */
#define ATA_SR_RDY  0x40   /* drive ready                 */
#define ATA_SR_BSY  0x80   /* drive busy                  */

/* ATA commands */
#define ATA_CMD_READ_PIO     0x20
#define ATA_CMD_WRITE_PIO    0x30
#define ATA_CMD_CACHE_FLUSH  0xE7
#define ATA_CMD_IDENTIFY     0xEC

/* IDENTIFY data words of interest */
#define IDENT_WORD_MODEL_START 27   /* words 27-46: model string (40 bytes) */
#define IDENT_WORD_LBA28_LO    60   /* LBA28 total sectors lo word          */
#define IDENT_WORD_LBA28_HI    61   /* LBA28 total sectors hi word          */

/* ============================================================
 *  Port I/O helpers
 * ============================================================ */

static inline unsigned char ata_inb(unsigned short port) {
    unsigned char v;
    __asm__ volatile ("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void ata_outb(unsigned short port, unsigned char v) {
    __asm__ volatile ("outb %0, %1" : : "a"(v), "Nd"(port));
}

static inline unsigned short ata_inw(unsigned short port) {
    unsigned short v;
    __asm__ volatile ("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline void ata_outw(unsigned short port, unsigned short v) {
    __asm__ volatile ("outw %0, %1" : : "a"(v), "Nd"(port));
}

/* 400 ns delay: read Alt Status four times (each read ≈ 100 ns on ISA). */
static void ata_delay400(void) {
    ata_inb(ATA_CTRL);
    ata_inb(ATA_CTRL);
    ata_inb(ATA_CTRL);
    ata_inb(ATA_CTRL);
}

/* ============================================================
 *  Polling helpers
 * ============================================================ */

/* Wait until BSY clears.  Returns 0 on success, -1 on timeout. */
static int ata_poll_bsy(void) {
    unsigned int i;
    for (i = 0; i < 0x100000U; i++) {
        if (!(ata_inb(ATA_BASE + ATA_REG_STATUS) & ATA_SR_BSY))
            return 0;
    }
    return -1;   /* timeout */
}

/* Wait until BSY=0 AND DRQ=1.  Returns 0 on success, -1 on error/timeout. */
static int ata_poll_drq(void) {
    unsigned int i;
    for (i = 0; i < 0x100000U; i++) {
        unsigned char s = ata_inb(ATA_BASE + ATA_REG_STATUS);
        if (s & (ATA_SR_ERR | ATA_SR_DF)) return -1;
        if (!(s & ATA_SR_BSY) && (s & ATA_SR_DRQ)) return 0;
    }
    return -1;   /* timeout */
}

/* ============================================================
 *  Drive state
 * ============================================================ */

static ata_drive_info_t ata_drives[2];

/* ============================================================
 *  IDENTIFY
 * ============================================================ */

static int ata_identify(int drive, ata_drive_info_t* info) {
    unsigned short buf[256];
    unsigned int   i;
    unsigned char  s;

    /* Select drive: 0xA0 = master, 0xB0 = slave */
    ata_outb(ATA_BASE + ATA_REG_DRIVE,
             (drive == ATA_DRIVE_SLAVE) ? 0xB0 : 0xA0);
    ata_delay400();

    /* Zero search registers (required before IDENTIFY) */
    ata_outb(ATA_BASE + ATA_REG_SECCOUNT, 0);
    ata_outb(ATA_BASE + ATA_REG_LBA0,     0);
    ata_outb(ATA_BASE + ATA_REG_LBA1,     0);
    ata_outb(ATA_BASE + ATA_REG_LBA2,     0);

    /* Send IDENTIFY command */
    ata_outb(ATA_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_delay400();

    /* Check if a drive is present at all */
    s = ata_inb(ATA_BASE + ATA_REG_STATUS);
    if (s == 0x00 || s == 0xFF) return -1;   /* floating / absent */

    /* Wait for BSY to clear */
    if (ata_poll_bsy() < 0) return -1;

    /* ATAPI / SATA devices set LBA1/LBA2 to non-zero — skip them for now */
    if (ata_inb(ATA_BASE + ATA_REG_LBA1) != 0 ||
        ata_inb(ATA_BASE + ATA_REG_LBA2) != 0)
        return -1;

    /* Wait for DRQ */
    if (ata_poll_drq() < 0) return -1;

    /* Read 256 × 16-bit words of IDENTIFY data */
    for (i = 0; i < 256; i++)
        buf[i] = ata_inw(ATA_BASE + ATA_REG_DATA);

    /* Model string: words 27–46, 40 bytes, each word is big-endian */
    for (i = 0; i < 20; i++) {
        info->model[i * 2]     = (char)(buf[IDENT_WORD_MODEL_START + i] >> 8);
        info->model[i * 2 + 1] = (char)(buf[IDENT_WORD_MODEL_START + i] & 0xFF);
    }
    info->model[40] = '\0';
    /* Trim trailing spaces */
    for (i = 39; i > 0 && info->model[i] == ' '; i--)
        info->model[i] = '\0';

    /* LBA28 total sector count (words 60–61, little-endian 32-bit) */
    info->sectors = (unsigned int)buf[IDENT_WORD_LBA28_LO] |
                    ((unsigned int)buf[IDENT_WORD_LBA28_HI] << 16);

    info->present = 1;
    return 0;
}

/* ============================================================
 *  Public API
 * ============================================================ */

void ata_init(void) {
    int d;

    ata_drives[0].present = 0;
    ata_drives[1].present = 0;

    /* Disable interrupts (nIEN=1) then issue software reset (SRST=1).
     * After clearing SRST, poll BSY until the controller is ready. */
    ata_outb(ATA_CTRL, 0x06);   /* nIEN=1, SRST=1 */
    ata_delay400();              /* hold SRST for ≥5 µs              */
    ata_outb(ATA_CTRL, 0x02);   /* nIEN=1, SRST=0                   */
    ata_poll_bsy();              /* wait until controller is ready    */

    for (d = 0; d < 2; d++)
        ata_identify(d, &ata_drives[d]);
}

int ata_is_present(int drive) {
    if (drive < 0 || drive > 1) return 0;
    return ata_drives[drive].present;
}

int ata_get_info(int drive, ata_drive_info_t* info) {
    if (drive < 0 || drive > 1 || !info) return -1;
    if (!ata_drives[drive].present) return -1;
    *info = ata_drives[drive];
    return 0;
}

int ata_read(int drive, uint32_t lba, uint8_t count, uint8_t* buf) {
    unsigned int i, j;
    unsigned short word;

    if (drive < 0 || drive > 1 || !buf || count == 0) return -1;
    if (!ata_drives[drive].present) return -1;

    /* Wait for BSY to clear before issuing a new command */
    if (ata_poll_bsy() < 0) return -1;

    /* LBA28 drive select:
       bits[7:5] = 111 (LBA mode, always-1, always-1)
       bit[4]    = drive select
       bits[3:0] = LBA[27:24]                                   */
    ata_outb(ATA_BASE + ATA_REG_DRIVE,
             (drive == ATA_DRIVE_SLAVE ? 0xF0 : 0xE0) |
             ((lba >> 24) & 0x0F));
    ata_delay400();   /* ATA spec: wait 400 ns after drive select  */

    ata_outb(ATA_BASE + ATA_REG_FEATURES, 0x00);
    ata_outb(ATA_BASE + ATA_REG_SECCOUNT, count);
    ata_outb(ATA_BASE + ATA_REG_LBA0, (uint8_t)(lba));
    ata_outb(ATA_BASE + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    ata_outb(ATA_BASE + ATA_REG_LBA2, (uint8_t)(lba >> 16));

    ata_outb(ATA_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (i = 0; i < (unsigned int)count; i++) {
        if (ata_poll_drq() < 0)
            return (i == 0) ? -1 : (int)i;

        /* Read 256 words = 512 bytes per sector */
        for (j = 0; j < 256; j++) {
            word = ata_inw(ATA_BASE + ATA_REG_DATA);
            buf[i * 512 + j * 2]     = (uint8_t)(word);
            buf[i * 512 + j * 2 + 1] = (uint8_t)(word >> 8);
        }

        ata_delay400();   /* required between sectors */
    }

    return (int)count;
}

int ata_write(int drive, uint32_t lba, uint8_t count, const uint8_t* buf) {
    unsigned int i, j;
    unsigned short word;

    if (drive < 0 || drive > 1 || !buf || count == 0) return -1;
    if (!ata_drives[drive].present) return -1;

    if (ata_poll_bsy() < 0) return -1;

    ata_outb(ATA_BASE + ATA_REG_DRIVE,
             (drive == ATA_DRIVE_SLAVE ? 0xF0 : 0xE0) |
             ((lba >> 24) & 0x0F));
    ata_delay400();   /* ATA spec: wait 400 ns after drive select  */

    ata_outb(ATA_BASE + ATA_REG_FEATURES, 0x00);
    ata_outb(ATA_BASE + ATA_REG_SECCOUNT, count);
    ata_outb(ATA_BASE + ATA_REG_LBA0, (uint8_t)(lba));
    ata_outb(ATA_BASE + ATA_REG_LBA1, (uint8_t)(lba >> 8));
    ata_outb(ATA_BASE + ATA_REG_LBA2, (uint8_t)(lba >> 16));

    ata_outb(ATA_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (i = 0; i < (unsigned int)count; i++) {
        if (ata_poll_drq() < 0)
            return (i == 0) ? -1 : (int)i;

        for (j = 0; j < 256; j++) {
            word = (unsigned short)buf[i * 512 + j * 2] |
                   ((unsigned short)buf[i * 512 + j * 2 + 1] << 8);
            ata_outw(ATA_BASE + ATA_REG_DATA, word);
        }

        ata_delay400();
    }

    /* Flush write cache */
    ata_outb(ATA_BASE + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_poll_bsy();

    return (int)count;
}

/* ============================================================
 *  IRQ14 handler (primary ATA, IRQ = 14, vector = 46)
 *  In PIO polling mode the IRQ is masked, but this stub is
 *  registered so spurious interrupts on real hardware are handled
 *  cleanly: just acknowledge both PICs and return.
 * ============================================================ */
unsigned int ata_irq14_handler(unsigned int current_esp) {
    /* Read status to clear pending interrupt on the drive side */
    (void)ata_inb(ATA_BASE + ATA_REG_STATUS);

    /* Send EOI to slave PIC, then master PIC */
    ata_outb(0xA0, 0x20);
    ata_outb(0x20, 0x20);

    return current_esp;
}
