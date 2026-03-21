#ifndef BLKDEV_H
#define BLKDEV_H

#include <stdint.h>

/* ============================================================
 *  Block device abstraction layer
 *
 *  Sits between low-level drivers (ATA, etc.) and higher-level
 *  consumers (FAT filesystem, etc.).
 *
 *  Devices are stored in a static table and addressed by integer
 *  index.  Partition sub-devices are registered automatically by
 *  blkdev_probe_partitions() after reading the MBR.
 *
 *  Naming convention:
 *    "hd0"   – first ATA drive (master)
 *    "hd1"   – second ATA drive (slave)
 *    "hd0p1" – first MBR partition of hd0
 *    "hd0p2" – second MBR partition of hd0, etc.
 * ============================================================ */

#define BLKDEV_MAX       16   /* max registered devices (disks + partitions) */
#define BLKDEV_NAME_MAX  16   /* max device name length including null        */

/* ---- Ops vtable ------------------------------------------------ */

typedef struct {
    /* Read 'count' blocks starting at 'lba' into 'buf'.
       Returns the number of blocks successfully read, or -1 on error. */
    int (*read) (void* priv, uint32_t lba, uint32_t count, uint8_t* buf);

    /* Write 'count' blocks starting at 'lba' from 'buf'.
       Returns the number of blocks successfully written, or -1 on error. */
    int (*write)(void* priv, uint32_t lba, uint32_t count, const uint8_t* buf);
} blkdev_ops_t;

/* ---- Device descriptor ----------------------------------------- */

typedef struct {
    char         name[BLKDEV_NAME_MAX]; /* e.g. "hd0", "hd0p1"          */
    uint32_t     block_size;            /* bytes per block (usually 512) */
    uint32_t     block_count;           /* total addressable blocks      */
    uint8_t      part_type;             /* 0 = whole disk; MBR type byte */
    int          parent;                /* -1 = root; >=0 = parent index */
    blkdev_ops_t ops;
    void*        priv;
    int          used;
} blkdev_t;

/* ---- Subsystem init -------------------------------------------- */

/* Zero the device table.  Must be called before any other blkdev_* call. */
void blkdev_init(void);

/* ---- Device registration --------------------------------------- */

/* Register an ATA drive (ATA_DRIVE_MASTER=0 / ATA_DRIVE_SLAVE=1) as a
   whole-disk block device ("hd0" / "hd1").
   Returns the new device index or -1 if the drive is absent / table full. */
int blkdev_register_ata(int ata_drive);

/* Low-level registration – used by drivers and blkdev_probe_partitions().
   Returns the new device index or -1 on error. */
int blkdev_register(const char* name,
                    uint32_t    block_size,
                    uint32_t    block_count,
                    uint8_t     part_type,
                    int         parent,
                    blkdev_ops_t ops,
                    void*       priv);

/* ---- Lookup ---------------------------------------------------- */

/* Number of currently registered devices. */
int blkdev_count(void);

/* Fill *out with the descriptor for device 'index'.
   Returns 0 on success, -1 if the slot is empty. */
int blkdev_get(int index, blkdev_t* out);

/* Find a device by name.  Returns its index or -1 if not found. */
int blkdev_find(const char* name);

/* ---- I/O ------------------------------------------------------- */

/* Read / write 'count' blocks starting at 'lba' through device 'index'.
   Returns blocks transferred on success or -1 on error. */
int blkdev_read (int index, uint32_t lba, uint32_t count, uint8_t* buf);
int blkdev_write(int index, uint32_t lba, uint32_t count, const uint8_t* buf);

/* ---- Partition probing ----------------------------------------- */

/* Read the MBR of device 'index', then register any non-empty primary
   partitions as child block devices ("<name>p1" … "<name>p4").
   Returns the number of partitions registered (0 if no valid MBR). */
int blkdev_probe_partitions(int index);

#endif /* BLKDEV_H */
