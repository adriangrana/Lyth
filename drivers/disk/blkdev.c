#include "blkdev.h"
#include "ata.h"
#include "string.h"

/* Forward declaration of the static device table used by cache helpers. */
static blkdev_t devices[BLKDEV_MAX];

/* ============================================================
 *  Block cache (write-through, direct mapped)
 *
 *  Goal: reduce redundant single-sector reads issued repeatedly
 *  by FAT metadata walkers (FAT table sectors, dir sectors, BPB).
 *
 *  Notes:
 *  - Enabled only for 512-byte block devices.
 *  - blkdev_write() is write-through and updates/invalidate cache.
 *  - Multi-sector reads/writes bypass per-sector cache fill to keep
 *    overhead low under streaming workloads.
 * ============================================================ */

#define BLK_CACHE_SIZE 128U

typedef struct {
    uint8_t  valid;
    int      dev_index;
    uint32_t lba;
    uint8_t  data[ATA_SECTOR_SIZE];
} blk_cache_entry_t;

static blk_cache_entry_t blk_cache[BLK_CACHE_SIZE];

static void blk_cache_copy_sector(uint8_t* dst, const uint8_t* src) {
    uint32_t i;
    for (i = 0; i < ATA_SECTOR_SIZE; i++) dst[i] = src[i];
}

static uint32_t blk_cache_hash(int dev_index, uint32_t lba) {
    uint32_t h = (uint32_t)dev_index * 2654435761U;
    h ^= lba * 2246822519U;
    return h & (BLK_CACHE_SIZE - 1U);
}

static void blk_cache_reset(void) {
    uint32_t i;
    for (i = 0; i < BLK_CACHE_SIZE; i++) {
        blk_cache[i].valid = 0;
        blk_cache[i].dev_index = -1;
        blk_cache[i].lba = 0;
    }
}

void blkdev_cache_invalidate_all(void) {
    blk_cache_reset();
}

void blkdev_cache_invalidate_device(int index) {
    uint32_t i;
    for (i = 0; i < BLK_CACHE_SIZE; i++) {
        if (blk_cache[i].valid && blk_cache[i].dev_index == index)
            blk_cache[i].valid = 0;
    }
}

static void blk_cache_invalidate_range(int index, uint32_t lba, uint32_t count) {
    uint32_t i;
    uint32_t end;

    if (count == 0) return;
    end = lba + count;

    for (i = 0; i < BLK_CACHE_SIZE; i++) {
        if (!blk_cache[i].valid || blk_cache[i].dev_index != index) continue;
        if (blk_cache[i].lba >= lba && blk_cache[i].lba < end)
            blk_cache[i].valid = 0;
    }
}

static int blk_cache_read_single(int index, uint32_t lba, uint8_t* buf) {
    uint32_t slot = blk_cache_hash(index, lba);
    blk_cache_entry_t* e = &blk_cache[slot];

    if (e->valid && e->dev_index == index && e->lba == lba) {
        blk_cache_copy_sector(buf, e->data);
        return 1;
    }

    if (devices[index].ops.read(devices[index].priv, lba, 1, e->data) != 1)
        return -1;

    e->valid = 1;
    e->dev_index = index;
    e->lba = lba;
    blk_cache_copy_sector(buf, e->data);
    return 1;
}

static void blk_cache_write_single(int index, uint32_t lba, const uint8_t* buf) {
    uint32_t slot = blk_cache_hash(index, lba);
    blk_cache_entry_t* e = &blk_cache[slot];
    e->valid = 1;
    e->dev_index = index;
    e->lba = lba;
    blk_cache_copy_sector(e->data, buf);
}

/* ============================================================
 *  Static device table
 * ============================================================ */

static int      device_count = 0;

/* ============================================================
 *  Partition private-data pool
 *
 *  We avoid dynamic allocation by using a static pool sized for
 *  all possible child devices.
 * ============================================================ */

#define BLKDEV_PART_MAX 32

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

static int append_uint_dec(char* dst, int pos, int cap, uint32_t value) {
    char tmp[16];
    int  n = 0;
    int  i;

    if (cap <= 0 || pos < 0 || pos >= cap) return pos;

    if (value == 0U) {
        if (pos + 1 < cap) dst[pos++] = '0';
        return pos;
    }

    while (value > 0U && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10U));
        value /= 10U;
    }

    for (i = n - 1; i >= 0; i--) {
        if (pos + 1 >= cap) break;
        dst[pos++] = tmp[i];
    }
    return pos;
}

static void build_part_name(char* out, int out_cap, const char* parent_name, uint32_t part_no) {
    int k = 0;
    int i;
    if (!out || out_cap <= 0 || !parent_name) return;
    for (i = 0; parent_name[i] && k + 2 < out_cap; i++) out[k++] = parent_name[i];
    if (k + 2 < out_cap) out[k++] = 'p';
    k = append_uint_dec(out, k, out_cap, part_no);
    out[k < out_cap ? k : out_cap - 1] = '\0';
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
    blk_cache_reset();
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
    uint32_t block_count;
    if (index < 0 || index >= BLKDEV_MAX || !devices[index].used) return -1;
    if (!buf || count == 0)                return 0;
    if (!devices[index].ops.read)          return -1;

    block_count = devices[index].block_count;
    if (lba >= block_count) return -1;
    if (count > block_count - lba) count = block_count - lba;

    if (count == 1U && devices[index].block_size == ATA_SECTOR_SIZE)
        return blk_cache_read_single(index, lba, buf);

    return devices[index].ops.read(devices[index].priv, lba, count, buf);
}

int blkdev_write(int index, uint32_t lba, uint32_t count, const uint8_t* buf) {
    int wrote;
    uint32_t block_count;
    if (index < 0 || index >= BLKDEV_MAX || !devices[index].used) return -1;
    if (!buf || count == 0)                return 0;
    if (!devices[index].ops.write)         return -1;

    block_count = devices[index].block_count;
    if (lba >= block_count) return -1;
    if (count > block_count - lba) count = block_count - lba;

    wrote = devices[index].ops.write(devices[index].priv, lba, count, buf);
    if (wrote <= 0) return wrote;

    if (devices[index].block_size == ATA_SECTOR_SIZE) {
        uint32_t i;
        for (i = 0; i < (uint32_t)wrote; i++) {
            blk_cache_write_single(index,
                                   lba + i,
                                   buf + i * ATA_SECTOR_SIZE);
        }
    } else {
        blk_cache_invalidate_range(index, lba, (uint32_t)wrote);
    }

    return wrote;
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

typedef struct __attribute__((packed)) {
    uint8_t  signature[8];      /* "EFI PART" */
    uint32_t revision;
    uint32_t header_size;
    uint32_t header_crc32;
    uint32_t reserved;
    uint64_t current_lba;
    uint64_t backup_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint8_t  disk_guid[16];
    uint64_t part_entries_lba;
    uint32_t num_part_entries;
    uint32_t part_entry_size;
    uint32_t part_entries_crc32;
} gpt_header_t;

typedef struct __attribute__((packed)) {
    uint8_t  type_guid[16];
    uint8_t  unique_guid[16];
    uint64_t first_lba;
    uint64_t last_lba;
    uint64_t attrs;
    uint16_t name[36];
} gpt_entry_min_t;

static int gpt_sig_ok(const uint8_t sig[8]) {
    return sig[0] == 'E' && sig[1] == 'F' && sig[2] == 'I' && sig[3] == ' ' &&
           sig[4] == 'P' && sig[5] == 'A' && sig[6] == 'R' && sig[7] == 'T';
}

static int guid_is_zero16(const uint8_t* g) {
    int i;
    for (i = 0; i < 16; i++) if (g[i] != 0U) return 0;
    return 1;
}

static int blkdev_probe_gpt(int index, const blkdev_t* parent_dev) {
    uint8_t       sec[512];
    gpt_header_t* h;
    uint64_t      tbl_lba;
    uint32_t      entries;
    uint32_t      ent_size;
    uint32_t      i;
    int           found = 0;

    if (blkdev_read(index, 1U, 1U, sec) != 1) return 0;

    h = (gpt_header_t*)(void*)sec;
    if (!gpt_sig_ok(h->signature)) return 0;
    if (h->header_size < 92U || h->header_size > 512U) return 0;
    if (h->part_entry_size < 128U || (h->part_entry_size % 8U) != 0U) return 0;
    if (h->num_part_entries == 0U) return 0;

    tbl_lba = h->part_entries_lba;
    ent_size = h->part_entry_size;
    entries = h->num_part_entries;

    if (tbl_lba == 0ULL || tbl_lba >= (uint64_t)parent_dev->block_count) return 0;

    for (i = 0; i < entries; i++) {
        uint64_t byte_off = (uint64_t)i * (uint64_t)ent_size;
        uint64_t lba = tbl_lba + (byte_off / 512ULL);
        uint32_t off = (uint32_t)(byte_off % 512ULL);
        gpt_entry_min_t e;
        char pname[BLKDEV_NAME_MAX];
        blkdev_ops_t ops;
        blkpart_ctx_t* ctx;
        uint32_t part_lba;
        uint32_t part_cnt;
        uint64_t first_lba;
        uint64_t last_lba;
        uint32_t j;

        if (off + ent_size > 512U) {
            /* Entry spans sectors; slow path copy */
            uint8_t tmp[256];
            uint32_t left = ent_size;
            uint32_t done = 0;
            uint64_t cur_lba = lba;
            uint32_t cur_off = off;

            if (ent_size > sizeof(tmp)) continue;

            while (left > 0U) {
                uint32_t chunk;
                if (blkdev_read(index, (uint32_t)cur_lba, 1U, sec) != 1) {
                    left = 0U;
                    done = 0U;
                    break;
                }
                chunk = 512U - cur_off;
                if (chunk > left) chunk = left;
                for (j = 0; j < chunk; j++) tmp[done + j] = sec[cur_off + j];
                done += chunk;
                left -= chunk;
                cur_lba++;
                cur_off = 0U;
            }
            if (done < 128U) continue;
            for (j = 0; j < 128U; j++) ((uint8_t*)(void*)&e)[j] = tmp[j];
        } else {
            if (blkdev_read(index, (uint32_t)lba, 1U, sec) != 1) continue;
            for (j = 0; j < 128U; j++) ((uint8_t*)(void*)&e)[j] = sec[off + j];
        }

        if (guid_is_zero16(e.type_guid)) continue; /* unused entry */
        if (e.first_lba == 0ULL || e.last_lba < e.first_lba) continue;

        first_lba = e.first_lba;
        last_lba  = e.last_lba;
        if (first_lba >= (uint64_t)parent_dev->block_count) continue;
        if (last_lba  >= (uint64_t)parent_dev->block_count) continue;

        part_lba = (uint32_t)first_lba;
        part_cnt = (uint32_t)(last_lba - first_lba + 1ULL);
        if (part_cnt == 0U) continue;
        if (part_lba + part_cnt > parent_dev->block_count) continue;
        if (part_pool_used >= BLKDEV_PART_MAX) break;

        build_part_name(pname, BLKDEV_NAME_MAX, parent_dev->name, i + 1U);

        ctx         = &part_pool[part_pool_used++];
        ctx->parent = index;
        ctx->offset = part_lba;
        ctx->size   = part_cnt;

        ops.read  = part_blkdev_read;
        ops.write = part_blkdev_write;

        if (blkdev_register(pname, parent_dev->block_size, part_cnt,
                            0xEEU, index, ops, ctx) >= 0)
            found++;
    }

    return found;
}

int blkdev_probe_partitions(int index) {
    uint8_t      mbr[512];
    mbr_entry_t* table;
    blkdev_t     parent_dev;
    int          found = 0;
    int          i;
    int          has_gpt = 0;

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
        if (table[i].type == 0xEEU) {
            has_gpt = 1;
            break;
        }
    }

    if (has_gpt) {
        int g = blkdev_probe_gpt(index, &parent_dev);
        if (g > 0) return g;
        /* If GPT header/table is invalid, fall back to classic MBR parse. */
    }

    for (i = 0; i < 4; i++) {
        char           pname[BLKDEV_NAME_MAX];
        blkdev_ops_t   ops;
        blkpart_ctx_t* ctx;

        if (table[i].type == 0x00)   continue;  /* empty slot     */
        if (table[i].lba_count == 0) continue;  /* zero-size      */

        /* Partition must be contained within the parent device */
        if (table[i].lba_start + table[i].lba_count > parent_dev.block_count)
            continue;

        if (part_pool_used >= BLKDEV_PART_MAX) break;

        /* Build partition name: "<parent_name>p<index>" */
        build_part_name(pname, BLKDEV_NAME_MAX, parent_dev.name, (uint32_t)(i + 1));

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
