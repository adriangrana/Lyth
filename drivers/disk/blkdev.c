#include "blkdev.h"
#include "ata.h"
#include "string.h"

/* ============================================================
 *  Static device table
 * ============================================================ */

static blkdev_t devices[BLKDEV_MAX];
static int      device_count = 0;

/* ============================================================
 *  Partition private-data pool
 *
 *  We avoid dynamic allocation by using a static pool sized for
 *  2 drives × 4 partitions each.
 * ============================================================ */

#define BLKDEV_PART_MAX 8

typedef struct {
    int      parent;   /* device index of the parent whole-disk device */
    uint32_t offset;   /* LBA offset of the partition within the parent */
    uint32_t size;     /* size in blocks                                */
} blkpart_ctx_t;

static blkpart_ctx_t part_pool[BLKDEV_PART_MAX];
static int           part_pool_used = 0;

/* ============================================================
 *  ATA ops
 * ============================================================ */

static int ata_blkdev_read(void* priv, uint32_t lba, uint32_t count, uint8_t* buf) {
    int      drive = (int)(uintptr_t)priv;
    uint32_t total = 0;

    while (count > 0) {
        /* ATA PIO takes uint8_t count; keep chunks ≤ 127 sectors */
        uint8_t  n   = (count > 127) ? 127 : (uint8_t)count;
        int      got = ata_read(drive, lba, n, buf);

        if (got < 0) return (total > 0) ? (int)total : -1;

        total += (uint32_t)got;
        lba   += (uint32_t)got;
        buf   += (uint32_t)got * ATA_SECTOR_SIZE;
        count -= (uint32_t)got;

        if ((uint32_t)got < n) break; /* short read – stop */
    }
    return (int)total;
}

static int ata_blkdev_write(void* priv, uint32_t lba, uint32_t count, const uint8_t* buf) {
    int      drive = (int)(uintptr_t)priv;
    uint32_t total = 0;

    while (count > 0) {
        uint8_t  n   = (count > 127) ? 127 : (uint8_t)count;
        int      got = ata_write(drive, lba, n, buf);

        if (got < 0) return (total > 0) ? (int)total : -1;

        total += (uint32_t)got;
        lba   += (uint32_t)got;
        buf   += (uint32_t)got * ATA_SECTOR_SIZE;
        count -= (uint32_t)got;

        if ((uint32_t)got < n) break;
    }
    return (int)total;
}

/* ============================================================
 *  Partition ops  (delegate to parent device with LBA offset)
 * ============================================================ */

static int part_blkdev_read(void* priv, uint32_t lba, uint32_t count, uint8_t* buf) {
    blkpart_ctx_t* ctx = (blkpart_ctx_t*)priv;
    if (lba >= ctx->size)            return -1;
    if (lba + count > ctx->size)     count = ctx->size - lba;
    return blkdev_read(ctx->parent, ctx->offset + lba, count, buf);
}

static int part_blkdev_write(void* priv, uint32_t lba, uint32_t count, const uint8_t* buf) {
    blkpart_ctx_t* ctx = (blkpart_ctx_t*)priv;
    if (lba >= ctx->size)            return -1;
    if (lba + count > ctx->size)     count = ctx->size - lba;
    return blkdev_write(ctx->parent, ctx->offset + lba, count, buf);
}

/* ============================================================
 *  Helpers
 * ============================================================ */

static void copy_name(char* dst, const char* src) {
    int i;
    for (i = 0; src[i] && i < BLKDEV_NAME_MAX - 1; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* ============================================================
 *  Public API
 * ============================================================ */

void blkdev_init(void) {
    int i;
    for (i = 0; i < BLKDEV_MAX; i++)
        devices[i].used = 0;
    device_count   = 0;
    part_pool_used = 0;
}

int blkdev_register(const char* name,
                    uint32_t    block_size,
                    uint32_t    block_count,
                    uint8_t     part_type,
                    int         parent,
                    blkdev_ops_t ops,
                    void*       priv) {
    int i;
    if (!name || block_size == 0 || block_count == 0) return -1;
    if (device_count >= BLKDEV_MAX) return -1;

    for (i = 0; i < BLKDEV_MAX; i++) {
        if (!devices[i].used) {
            copy_name(devices[i].name, name);
            devices[i].block_size  = block_size;
            devices[i].block_count = block_count;
            devices[i].part_type   = part_type;
            devices[i].parent      = parent;
            devices[i].ops         = ops;
            devices[i].priv        = priv;
            devices[i].used        = 1;
            device_count++;
            return i;
        }
    }
    return -1;
}

int blkdev_register_ata(int ata_drive) {
    char          name[BLKDEV_NAME_MAX];
    blkdev_ops_t  ops;
    ata_drive_info_t info;

    if (!ata_is_present(ata_drive))          return -1;
    if (ata_get_info(ata_drive, &info) < 0)  return -1;

    name[0] = 'h';
    name[1] = 'd';
    name[2] = (char)('0' + ata_drive);
    name[3] = '\0';

    ops.read  = ata_blkdev_read;
    ops.write = ata_blkdev_write;

    return blkdev_register(name, ATA_SECTOR_SIZE, info.sectors,
                           0, -1, ops, (void*)(uintptr_t)ata_drive);
}

int blkdev_count(void) {
    return device_count;
}

int blkdev_get(int index, blkdev_t* out) {
    if (index < 0 || index >= BLKDEV_MAX || !out) return -1;
    if (!devices[index].used)                      return -1;
    *out = devices[index];
    return 0;
}

int blkdev_find(const char* name) {
    int i;
    if (!name) return -1;
    for (i = 0; i < BLKDEV_MAX; i++) {
        if (devices[i].used && str_equals(devices[i].name, name))
            return i;
    }
    return -1;
}

int blkdev_read(int index, uint32_t lba, uint32_t count, uint8_t* buf) {
    if (index < 0 || index >= BLKDEV_MAX || !devices[index].used) return -1;
    if (!buf || count == 0)                return 0;
    if (!devices[index].ops.read)          return -1;
    return devices[index].ops.read(devices[index].priv, lba, count, buf);
}

int blkdev_write(int index, uint32_t lba, uint32_t count, const uint8_t* buf) {
    if (index < 0 || index >= BLKDEV_MAX || !devices[index].used) return -1;
    if (!buf || count == 0)                return 0;
    if (!devices[index].ops.write)         return -1;
    return devices[index].ops.write(devices[index].priv, lba, count, buf);
}

/* ============================================================
 *  MBR partition probing
 * ============================================================ */

/* Standard MBR partition entry (16 bytes, packed). */
typedef struct __attribute__((packed)) {
    uint8_t  status;       /* 0x80 = bootable                   */
    uint8_t  chs_first[3];
    uint8_t  type;         /* partition type code               */
    uint8_t  chs_last[3];
    uint32_t lba_start;    /* first sector (LBA)                */
    uint32_t lba_count;    /* total sectors in partition        */
} mbr_entry_t;

int blkdev_probe_partitions(int index) {
    uint8_t      mbr[512];
    mbr_entry_t* table;
    blkdev_t     parent_dev;
    int          found = 0;
    int          i;

    if (blkdev_get(index, &parent_dev) < 0)      return 0;
    if (blkdev_read(index, 0, 1, mbr) != 1)      return 0;

    /* Validate MBR signature */
    if (mbr[510] != 0x55 || mbr[511] != 0xAA)   return 0;

    /* If bytes 11-12 read as little-endian 512 (0x0200), this is a FAT/NTFS
       VBR (BPB bytes_per_sector field), not a real MBR with a partition table.
       A genuine MBR has arbitrary boot code at those bytes, never 0x0200. */
    {
        uint16_t bps = (uint16_t)mbr[11] | ((uint16_t)mbr[12] << 8);
        if (bps == 512) return 0;
    }

    table = (mbr_entry_t*)(mbr + 446);

    for (i = 0; i < 4; i++) {
        char           pname[BLKDEV_NAME_MAX];
        blkdev_ops_t   ops;
        blkpart_ctx_t* ctx;
        int            k;

        if (table[i].type == 0x00)   continue;  /* empty slot     */
        if (table[i].lba_count == 0) continue;  /* zero-size      */

        /* Partition must be contained within the parent device */
        if (table[i].lba_start + table[i].lba_count > parent_dev.block_count)
            continue;

        if (part_pool_used >= BLKDEV_PART_MAX) break;

        /* Build partition name: "<parent_name>p<1..4>" */
        for (k = 0; parent_dev.name[k] && k < BLKDEV_NAME_MAX - 3; k++)
            pname[k] = parent_dev.name[k];
        pname[k++] = 'p';
        pname[k++] = (char)('1' + i);
        pname[k]   = '\0';

        ctx         = &part_pool[part_pool_used++];
        ctx->parent = index;
        ctx->offset = table[i].lba_start;
        ctx->size   = table[i].lba_count;

        ops.read  = part_blkdev_read;
        ops.write = part_blkdev_write;

        if (blkdev_register(pname, parent_dev.block_size, table[i].lba_count,
                            table[i].type, index, ops, ctx) >= 0)
            found++;
    }
    return found;
}
