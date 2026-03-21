#include "fat16.h"
#include "blkdev.h"
#include "heap.h"
#include "string.h"
#include <stdint.h>

/* ============================================================
 *  FAT16 on-disk structures
 * ============================================================ */

/* BIOS Parameter Block — FAT16 layout */
typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t sectors_per_fat;       /* 0 on FAT32 – used to reject FAT32 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* Extended BPB (present when boot_signature == 0x29) */
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];            /* "FAT16   " */
} fat16_bpb_t;

/* 32-byte directory entry */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;      /* always 0 on FAT16 */
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t first_cluster;
    uint32_t size;
} fat16_dirent_t;

#define FAT_ATTR_READONLY   0x01
#define FAT_ATTR_HIDDEN     0x02
#define FAT_ATTR_SYSTEM     0x04
#define FAT_ATTR_VOLID      0x08
#define FAT_ATTR_DIR        0x10
#define FAT_ATTR_ARCHIVE    0x20
#define FAT_ATTR_LFN        0x0F    /* long-filename marker */

#define FAT16_EOC_MIN       0xFFF8U /* ≥ this → end of cluster chain */
#define FAT16_BAD_CLUSTER   0xFFF7U

/* ============================================================
 *  Volume descriptor – static pool (one entry per mounted volume)
 * ============================================================ */

typedef struct {
    int      blkdev_idx;
    uint16_t bytes_per_sector;    /* must be 512 */
    uint8_t  sectors_per_cluster;
    uint32_t cluster_size;        /* = bytes_per_sector × sectors_per_cluster */
    uint32_t fat_lba;             /* first sector of FAT1 (relative to blkdev) */
    uint32_t root_lba;            /* first sector of root directory */
    uint16_t root_entry_count;
    uint32_t data_lba;            /* first sector of cluster 2 */
    uint8_t  fat_count;           /* number of FAT copies (usually 2) */
    uint16_t sectors_per_fat;     /* sectors per FAT copy */
    int      used;
} fat16_vol_t;

static fat16_vol_t vol_pool[FAT16_MAX_VOLUMES];

/* ============================================================
 *  Per-node private data
 *
 *  For the root directory the priv is embedded inside fat16_vol_t
 *  (to avoid a heap allocation that is never freed).
 *  For every other node we allocate a combined fat16_node_t on the
 *  heap so that a single kfree() releases both the vfs_node_t and
 *  the priv – which is important because vfs_resolve() frees
 *  intermediate dynamic nodes with a raw kfree(), not via close().
 * ============================================================ */

typedef struct {
    fat16_vol_t* vol;
    uint16_t     first_cluster;  /* 0 = root directory (fixed region) */
    uint32_t     size;           /* file size in bytes; 0 for directories */
    uint32_t     dirent_lba;     /* sector LBA of this node's directory entry */
    uint32_t     dirent_idx;     /* entry index within that sector */
} fat16_priv_t;

/* Combined heap-allocated node + priv (vfs_node_t MUST be first). */
typedef struct {
    vfs_node_t   node;
    fat16_priv_t priv;
} fat16_node_t;

/* Root node pool – static, NOT VFS_FLAG_DYNAMIC. */
static vfs_node_t    root_node_pool[FAT16_MAX_VOLUMES];
static fat16_priv_t  root_priv_pool[FAT16_MAX_VOLUMES];

/* ============================================================
 *  LFN structures and helpers
 * ============================================================ */

/* LFN directory entry (attr == FAT_ATTR_LFN == 0x0F) */
typedef struct __attribute__((packed)) {
    uint8_t  order;       /* seq num; bit 6 (0x40) set on first entry */
    uint16_t name1[5];    /* UTF-16LE chars  1-5  */
    uint8_t  attr;        /* always 0x0F */
    uint8_t  type;        /* always 0x00 */
    uint8_t  checksum;    /* checksum of the 8.3 entry that follows */
    uint16_t name2[6];    /* UTF-16LE chars  6-11 */
    uint16_t first_cl;    /* always 0x0000 */
    uint16_t name3[2];    /* UTF-16LE chars 12-13 */
} fat_lfn_entry_t;

typedef struct {
    char buf[256];  /* assembled LFN (NUL-terminated) */
    int  valid;     /* 1 = buf holds a pending LFN */
} fat_lfn_state_t;

static void lfn_state_reset(fat_lfn_state_t* st) {
    st->valid  = 0;
    st->buf[0] = '\0';
}

/* Extract up to 13 UTF-16LE chars from one LFN entry into dst[base..]. */
static void lfn_extract_13(const fat_lfn_entry_t* e, char* dst, int base) {
    int pos = base;
    int k;
    for (k = 0; k < 5 && pos < 255; k++) {
        uint16_t ch = e->name1[k];
        if (ch == 0x0000U || ch == 0xFFFFU) { dst[pos] = '\0'; return; }
        dst[pos++] = (char)(ch & 0x7FU);
    }
    for (k = 0; k < 6 && pos < 255; k++) {
        uint16_t ch = e->name2[k];
        if (ch == 0x0000U || ch == 0xFFFFU) { dst[pos] = '\0'; return; }
        dst[pos++] = (char)(ch & 0x7FU);
    }
    for (k = 0; k < 2 && pos < 255; k++) {
        uint16_t ch = e->name3[k];
        if (ch == 0x0000U || ch == 0xFFFFU) { dst[pos] = '\0'; return; }
        dst[pos++] = (char)(ch & 0x7FU);
    }
}

/* Feed one LFN entry into the accumulator state. */
static void lfn_state_feed(fat_lfn_state_t* st, const fat_lfn_entry_t* e) {
    int order = (int)(e->order & 0x3FU);
    int slot  = order - 1;
    int z;
    if (slot < 0 || slot >= 20) { st->valid = 0; return; }
    if (e->order & 0x40U) {
        for (z = 0; z < 256; z++) st->buf[z] = '\0';
        st->valid = 1;
    }
    if (!st->valid) return;
    lfn_extract_13(e, st->buf, slot * 13);
}

/* ============================================================
 *  Low-level I/O
 * ============================================================ */

/* Read one 512-byte sector from the blkdev into buf. */
static int fat16_read_sector(fat16_vol_t* vol, uint32_t lba, uint8_t* buf) {
    return (blkdev_read(vol->blkdev_idx, lba, 1, buf) == 1) ? 0 : -1;
}

/* Return the FAT16 entry for 'cluster' (i.e. the next cluster in the
   chain), or 0xFFFF on I/O error or out-of-range. */
static uint16_t fat16_next_cluster(fat16_vol_t* vol, uint16_t cluster) {
    uint8_t  sec[512];
    uint32_t byte_off;
    uint32_t fat_sec;
    uint32_t sec_off;
    uint16_t val;

    byte_off = (uint32_t)cluster * 2U;
    fat_sec  = vol->fat_lba + byte_off / (uint32_t)vol->bytes_per_sector;
    sec_off  = byte_off % (uint32_t)vol->bytes_per_sector;

    if (fat16_read_sector(vol, fat_sec, sec) < 0) return 0xFFFF;

    /* Bounds check: sec_off + 1 must be < 512 */
    if (sec_off + 1U >= (uint32_t)vol->bytes_per_sector) return 0xFFFF;

    val = (uint16_t)((uint16_t)sec[sec_off] | ((uint16_t)sec[sec_off + 1U] << 8));
    return val;
}

/* Read one full cluster (cluster_size bytes) into buf. */
static int fat16_read_cluster(fat16_vol_t* vol, uint16_t cluster, uint8_t* buf) {
    uint32_t lba = vol->data_lba + (uint32_t)(cluster - 2U) * vol->sectors_per_cluster;
    uint32_t s;

    for (s = 0; s < (uint32_t)vol->sectors_per_cluster; s++) {
        if (fat16_read_sector(vol, lba + s,
                              buf + s * (uint32_t)vol->bytes_per_sector) < 0)
            return -1;
    }
    return 0;
}

/* ============================================================
 *  Low-level write helpers
 * ============================================================ */

/* Write one 512-byte sector to the blkdev. */
static int fat16_write_sector(fat16_vol_t* vol, uint32_t lba, const uint8_t* buf) {
    return (blkdev_write(vol->blkdev_idx, lba, 1, buf) == 1) ? 0 : -1;
}

/* Write 'val' into the FAT entry for 'cluster' in every FAT copy. */
static int fat16_write_fat_entry(fat16_vol_t* vol, uint16_t cluster, uint16_t val) {
    uint8_t  sec[512];
    uint32_t byte_off = (uint32_t)cluster * 2U;
    uint32_t fat_sec  = byte_off / (uint32_t)vol->bytes_per_sector;
    uint32_t sec_off  = byte_off % (uint32_t)vol->bytes_per_sector;
    uint32_t copy;

    if (sec_off + 1U >= (uint32_t)vol->bytes_per_sector) return -1;

    for (copy = 0; copy < (uint32_t)vol->fat_count; copy++) {
        uint32_t lba = vol->fat_lba + copy * (uint32_t)vol->sectors_per_fat + fat_sec;
        if (fat16_read_sector(vol, lba, sec) < 0) return -1;
        sec[sec_off]     = (uint8_t)(val & 0xFFU);
        sec[sec_off + 1] = (uint8_t)((val >> 8) & 0xFFU);
        if (fat16_write_sector(vol, lba, sec) < 0) return -1;
    }
    return 0;
}

/* Scan FAT1 for a free cluster (entry == 0x0000), mark it as EOC (0xFFFF),
   and return its cluster number.  Returns 0 if none available. */
static uint16_t fat16_alloc_cluster(fat16_vol_t* vol) {
    uint8_t  sec[512];
    uint32_t bps            = (uint32_t)vol->bytes_per_sector;
    uint32_t entries_per_s  = bps / 2U;
    uint32_t fat_sectors    = (uint32_t)vol->sectors_per_fat;
    uint32_t s;
    uint32_t i;

    for (s = 0; s < fat_sectors; s++) {
        if (fat16_read_sector(vol, vol->fat_lba + s, sec) < 0) continue;
        for (i = 0; i < entries_per_s; i++) {
            uint16_t entry = (uint16_t)((uint16_t)sec[i * 2U]
                                        | ((uint16_t)sec[i * 2U + 1U] << 8));
            if (entry == 0x0000U) {
                uint16_t cluster = (uint16_t)(s * entries_per_s + i);
                if (cluster < 2U) continue; /* clusters 0 and 1 are reserved */
                if (fat16_write_fat_entry(vol, cluster, 0xFFFFU) < 0) return 0;
                return cluster;
            }
        }
    }
    return 0;
}

/* Free the entire cluster chain starting at first_cluster. */
static void fat16_free_cluster_chain(fat16_vol_t* vol, uint16_t first_cluster) {
    uint16_t cluster = first_cluster;
    while (cluster >= 2U && cluster < FAT16_EOC_MIN) {
        uint16_t next = fat16_next_cluster(vol, cluster);
        fat16_write_fat_entry(vol, cluster, 0x0000U);
        cluster = next;
    }
}

/* Write one full cluster (cluster_size bytes) from buf. */
static int fat16_write_cluster(fat16_vol_t* vol, uint16_t cluster, const uint8_t* buf) {
    uint32_t lba = vol->data_lba + (uint32_t)(cluster - 2U) * vol->sectors_per_cluster;
    uint32_t s;

    for (s = 0; s < (uint32_t)vol->sectors_per_cluster; s++) {
        if (fat16_write_sector(vol, lba + s,
                               buf + s * (uint32_t)vol->bytes_per_sector) < 0)
            return -1;
    }
    return 0;
}

/* Update the first_cluster and size fields of the dirent at (dirent_lba, dirent_idx). */
static int fat16_update_dirent(fat16_vol_t* vol,
                                uint32_t dirent_lba, uint32_t dirent_idx,
                                uint16_t first_cluster, uint32_t file_size) {
    uint8_t         sec[512];
    fat16_dirent_t* ent;

    if (fat16_read_sector(vol, dirent_lba, sec) < 0) return -1;
    ent = (fat16_dirent_t*)(void*)sec;
    ent[dirent_idx].first_cluster    = first_cluster;
    ent[dirent_idx].first_cluster_hi = 0;
    ent[dirent_idx].size             = file_size;
    return fat16_write_sector(vol, dirent_lba, sec);
}

/* ============================================================
 *  Name formatting
 * ============================================================ */

/* Convert an 8.3 FAT entry to a NUL-terminated lowercase string.
   "HELLO   TXT" → "hello.txt"
   "README  " (no ext or ext all spaces) → "readme"
   dst must be at least 13 bytes. */
static void fat16_format_name(const uint8_t* name8, const uint8_t* ext3, char* dst) {
    int nlen, elen, i, j = 0;

    /* Trim trailing spaces from name part */
    for (nlen = 8; nlen > 0 && name8[nlen - 1] == ' '; nlen--)
        ;

    for (i = 0; i < nlen; i++) {
        uint8_t c = name8[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        dst[j++] = (char)c;
    }

    /* Trim trailing spaces from extension part */
    for (elen = 3; elen > 0 && ext3[elen - 1] == ' '; elen--)
        ;

    if (elen > 0) {
        dst[j++] = '.';
        for (i = 0; i < elen; i++) {
            uint8_t c = ext3[i];
            if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
            dst[j++] = (char)c;
        }
    }

    dst[j] = '\0';
}

/* True if the VFS name (case-insensitive) matches the 8.3 FAT entry. */
static int fat16_name_match(const char* vfs_name,
                            const uint8_t* name8, const uint8_t* ext3) {
    char formatted[13];
    fat16_format_name(name8, ext3, formatted);
    return str_equals_ignore_case(vfs_name, formatted);
}

/* ============================================================
 *  Directory walker
 *
 *  Iterates over all valid entries in the directory whose first
 *  cluster is first_cluster (0 = FAT16 fixed root-dir region).
 *  Skips: deleted (0xE5), end-of-table (0x00), LFN (attr==0x0F),
 *  volume labels, and "." / ".." entries.
 *
 *  Calls cb(cookie, entry) for each surviving entry.
 *  Stops and returns the callback's non-zero return value, or 0
 *  when all entries have been visited.
 * ============================================================ */

typedef int (*fat16_dir_cb)(void* cookie, const fat16_dirent_t* e, const char* lfn);

static int fat16_walk_dir(fat16_vol_t* vol, uint16_t first_cluster,
                          fat16_dir_cb cb, void* cookie) {
    const uint32_t entries_per_sector =
        (uint32_t)vol->bytes_per_sector / sizeof(fat16_dirent_t);

    if (first_cluster == 0) {
        /* ---- Fixed root directory region ---- */
        fat_lfn_state_t lfn;
        uint32_t total_sectors = ((uint32_t)vol->root_entry_count * 32U +
                                  (uint32_t)vol->bytes_per_sector - 1U)
                                 / (uint32_t)vol->bytes_per_sector;
        uint32_t s;
        uint8_t  sec[512];

        lfn_state_reset(&lfn);
        for (s = 0; s < total_sectors; s++) {
            fat16_dirent_t* ent;
            uint32_t i;

            if (fat16_read_sector(vol, vol->root_lba + s, sec) < 0) return 0;
            ent = (fat16_dirent_t*)(void*)sec;

            for (i = 0; i < entries_per_sector; i++) {
                if (ent[i].name[0] == 0x00) return 0;      /* end of table */
                if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn_state_reset(&lfn); continue; }
                if (ent[i].attr == FAT_ATTR_LFN) {
                    lfn_state_feed(&lfn, (const fat_lfn_entry_t*)(const void*)&ent[i]);
                    continue;
                }
                if (ent[i].attr & FAT_ATTR_VOLID) { lfn_state_reset(&lfn); continue; }
                if (ent[i].name[0] == '.') { lfn_state_reset(&lfn); continue; }

                {
                    int r = cb(cookie, &ent[i], lfn.valid ? lfn.buf : (const char*)0);
                    lfn_state_reset(&lfn);
                    if (r) return r;
                }
            }
        }
        return 0;
    } else {
        /* ---- Cluster-chain subdirectory ---- */
        uint8_t* cbuf;
        uint16_t cluster = first_cluster;
        fat_lfn_state_t lfn;

        if (vol->cluster_size == 0) return 0;
        cbuf = (uint8_t*)kmalloc(vol->cluster_size);
        if (!cbuf) return 0;

        lfn_state_reset(&lfn);
        while (cluster >= 2U && cluster < FAT16_EOC_MIN) {
            fat16_dirent_t* ent;
            uint32_t        eps = vol->cluster_size / sizeof(fat16_dirent_t);
            uint32_t        i;

            if (fat16_read_cluster(vol, cluster, cbuf) < 0) break;
            ent = (fat16_dirent_t*)(void*)cbuf;

            for (i = 0; i < eps; i++) {
                if (ent[i].name[0] == 0x00) { kfree(cbuf); return 0; }
                if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn_state_reset(&lfn); continue; }
                if (ent[i].attr == FAT_ATTR_LFN) {
                    lfn_state_feed(&lfn, (const fat_lfn_entry_t*)(const void*)&ent[i]);
                    continue;
                }
                if (ent[i].attr & FAT_ATTR_VOLID) { lfn_state_reset(&lfn); continue; }
                if (ent[i].name[0] == '.') { lfn_state_reset(&lfn); continue; }

                {
                    int r = cb(cookie, &ent[i], lfn.valid ? lfn.buf : (const char*)0);
                    lfn_state_reset(&lfn);
                    if (r) { kfree(cbuf); return r; }
                }
            }

            cluster = fat16_next_cluster(vol, cluster);
        }

        kfree(cbuf);
        return 0;
    }
}

/* ============================================================
 *  Located directory walker
 *
 *  Like fat16_walk_dir but also passes the on-disk sector LBA and
 *  entry index within that sector to the callback.  Used by write
 *  helpers that need to know where a dirent lives on disk.
 * ============================================================ */

typedef int (*fat16_dir_cb_loc)(void* cookie, const fat16_dirent_t* e,
                                uint32_t lba, uint32_t idx, const char* lfn);

static int fat16_walk_dir_loc(fat16_vol_t* vol, uint16_t first_cluster,
                               fat16_dir_cb_loc cb, void* cookie) {
    const uint32_t bps = (uint32_t)vol->bytes_per_sector;
    const uint32_t eps = bps / sizeof(fat16_dirent_t);

    if (first_cluster == 0) {
        uint32_t total_s = ((uint32_t)vol->root_entry_count * 32U + bps - 1U) / bps;
        uint32_t s;
        uint8_t  sec[512];
        fat_lfn_state_t lfn;
        lfn_state_reset(&lfn);

        for (s = 0; s < total_s; s++) {
            fat16_dirent_t* ent;
            uint32_t i;
            uint32_t lba = vol->root_lba + s;

            if (fat16_read_sector(vol, lba, sec) < 0) return 0;
            ent = (fat16_dirent_t*)(void*)sec;

            for (i = 0; i < eps; i++) {
                if (ent[i].name[0] == 0x00) return 0;
                if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn_state_reset(&lfn); continue; }
                if (ent[i].attr == FAT_ATTR_LFN) {
                    lfn_state_feed(&lfn, (const fat_lfn_entry_t*)(const void*)&ent[i]);
                    continue;
                }
                if (ent[i].attr & FAT_ATTR_VOLID) { lfn_state_reset(&lfn); continue; }
                if (ent[i].name[0] == '.') { lfn_state_reset(&lfn); continue; }
                {
                    int r = cb(cookie, &ent[i], lba, i, lfn.valid ? lfn.buf : (const char*)0);
                    lfn_state_reset(&lfn);
                    if (r) return r;
                }
            }
        }
        return 0;
    } else {
        uint8_t* cbuf;
        uint16_t cluster = first_cluster;
        fat_lfn_state_t lfn;

        if (vol->cluster_size == 0) return 0;
        cbuf = (uint8_t*)kmalloc(vol->cluster_size);
        if (!cbuf) return 0;
        lfn_state_reset(&lfn);

        while (cluster >= 2U && cluster < FAT16_EOC_MIN) {
            fat16_dirent_t* ent;
            uint32_t        total_e  = vol->cluster_size / sizeof(fat16_dirent_t);
            uint32_t        base_lba = vol->data_lba
                                       + (uint32_t)(cluster - 2U)
                                       * vol->sectors_per_cluster;
            uint32_t        i;

            if (fat16_read_cluster(vol, cluster, cbuf) < 0) break;
            ent = (fat16_dirent_t*)(void*)cbuf;

            for (i = 0; i < total_e; i++) {
                uint32_t sec_i = i / eps;
                uint32_t ent_i = i % eps;
                uint32_t lba   = base_lba + sec_i;

                if (ent[i].name[0] == 0x00) { kfree(cbuf); return 0; }
                if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn_state_reset(&lfn); continue; }
                if (ent[i].attr == FAT_ATTR_LFN) {
                    lfn_state_feed(&lfn, (const fat_lfn_entry_t*)(const void*)&ent[i]);
                    continue;
                }
                if (ent[i].attr & FAT_ATTR_VOLID) { lfn_state_reset(&lfn); continue; }
                if (ent[i].name[0] == '.') { lfn_state_reset(&lfn); continue; }
                {
                    int r = cb(cookie, &ent[i], lba, ent_i, lfn.valid ? lfn.buf : (const char*)0);
                    lfn_state_reset(&lfn);
                    if (r) { kfree(cbuf); return r; }
                }
            }

            cluster = fat16_next_cluster(vol, cluster);
        }

        kfree(cbuf);
        return 0;
    }
}

/* ============================================================
 *  Walker callbacks
 * ============================================================ */

typedef struct {
    unsigned int idx;       /* target index */
    unsigned int cur;       /* current count */
    char*        out;
    unsigned int out_max;
    int          found;
} fat16_readdir_ctx_t;

static int fat16_readdir_cb(void* cookie, const fat16_dirent_t* e, const char* lfn) {
    fat16_readdir_ctx_t* ctx = (fat16_readdir_ctx_t*)cookie;

    if (ctx->cur == ctx->idx) {
        if (lfn && lfn[0]) {
            unsigned int i = 0;
            while (lfn[i] && i < ctx->out_max - 1U) { ctx->out[i] = lfn[i]; i++; }
            ctx->out[i] = '\0';
        } else {
            fat16_format_name(e->name, e->ext, ctx->out);
        }
        ctx->found = 1;
        return 1;   /* stop */
    }
    ctx->cur++;
    return 0;
}

/* Located finddir context — also captures on-disk position of the dirent. */
typedef struct {
    const char*    target;
    fat16_dirent_t result;
    uint32_t       dirent_lba;
    uint32_t       dirent_idx;
    int            found;
} fat16_finddir_loc_ctx_t;

static int fat16_finddir_loc_cb(void* cookie, const fat16_dirent_t* e,
                                 uint32_t lba, uint32_t idx, const char* lfn) {
    fat16_finddir_loc_ctx_t* ctx = (fat16_finddir_loc_ctx_t*)cookie;
    int match = 0;
    if (lfn && lfn[0]) match = str_equals_ignore_case(ctx->target, lfn);
    if (!match)        match = fat16_name_match(ctx->target, e->name, e->ext);
    if (match) {
        ctx->result     = *e;
        ctx->dirent_lba = lba;
        ctx->dirent_idx = idx;
        ctx->found      = 1;
        return 1;   /* stop */
    }
    return 0;
}

/* ============================================================
 *  VFS vtable declarations (definitions follow below)
 * ============================================================ */

static int         fat16_dir_readdir(vfs_node_t*, unsigned int, char*, unsigned int);
static vfs_node_t* fat16_dir_finddir(vfs_node_t*, const char*);
static int         fat16_file_read  (vfs_node_t*, unsigned int, unsigned int, unsigned char*);
static int         fat16_file_write (vfs_node_t*, unsigned int, unsigned int, const unsigned char*);
static vfs_node_t* fat16_dir_create (vfs_node_t*, const char*, unsigned int);
static int         fat16_dir_unlink (vfs_node_t*, const char*);

static vfs_ops_t fat16_file_ops = {
    .read    = fat16_file_read,
    .write   = fat16_file_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,   /* vfs.c frees node via kfree(node) when VFS_FLAG_DYNAMIC */
    .create  = 0,
    .unlink  = 0,
};

static vfs_ops_t fat16_dir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = fat16_dir_readdir,
    .finddir = fat16_dir_finddir,
    .open    = 0,
    .close   = 0,
    .create  = fat16_dir_create,
    .unlink  = fat16_dir_unlink,
};

/* ============================================================
 *  VFS ops: directory
 * ============================================================ */

static int fat16_dir_readdir(vfs_node_t* node, unsigned int index,
                              char* name_out, unsigned int name_max) {
    fat16_priv_t*       priv = (fat16_priv_t*)node->impl;
    fat16_readdir_ctx_t ctx;

    ctx.idx   = index;
    ctx.cur   = 0;
    ctx.out   = name_out;
    ctx.out_max = name_max;
    ctx.found = 0;

    fat16_walk_dir(priv->vol, priv->first_cluster, fat16_readdir_cb, &ctx);
    return ctx.found ? 0 : -1;
}

static vfs_node_t* fat16_dir_finddir(vfs_node_t* node, const char* name) {
    fat16_priv_t*           priv = (fat16_priv_t*)node->impl;
    fat16_finddir_loc_ctx_t ctx;
    fat16_node_t*           fn;
    unsigned int            i;

    ctx.target = name;
    ctx.found  = 0;

    fat16_walk_dir_loc(priv->vol, priv->first_cluster, fat16_finddir_loc_cb, &ctx);
    if (!ctx.found) return 0;

    /* Allocate node + priv in one block so a single kfree() cleans both. */
    fn = (fat16_node_t*)kmalloc(sizeof(fat16_node_t));
    if (!fn) return 0;

    fn->priv.vol           = priv->vol;
    fn->priv.first_cluster = ctx.result.first_cluster;
    fn->priv.size          = ctx.result.size;
    fn->priv.dirent_lba    = ctx.dirent_lba;
    fn->priv.dirent_idx    = ctx.dirent_idx;

    /* name is always the caller-supplied string for case-preserving behaviour. */
    for (i = 0; name[i] && i < VFS_NAME_MAX - 1U; i++)
        fn->node.name[i] = name[i];
    fn->node.name[i] = '\0';

    fn->node.impl       = &fn->priv;
    fn->node.mountpoint = 0;

    if (ctx.result.attr & FAT_ATTR_DIR) {
        fn->node.flags = VFS_FLAG_DIR  | VFS_FLAG_DYNAMIC;
        fn->node.size  = 0;
        fn->node.ops   = &fat16_dir_ops;
    } else {
        fn->node.flags = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
        fn->node.size  = ctx.result.size;
        fn->node.ops   = &fat16_file_ops;
    }

    return &fn->node;
}

/* ============================================================
 *  VFS ops: file read
 * ============================================================ */

static int fat16_file_read(vfs_node_t* node, unsigned int offset,
                           unsigned int size, unsigned char* buf) {
    fat16_priv_t* priv        = (fat16_priv_t*)node->impl;
    fat16_vol_t*  vol         = priv->vol;
    uint32_t      file_size   = priv->size;
    uint32_t      cluster_sz  = vol->cluster_size;
    uint32_t      to_read, written, start_cl_idx, i;
    uint16_t      cluster;
    uint8_t*      tmp;

    if (offset >= file_size || size == 0) return 0;

    to_read = file_size - offset;
    if (to_read > (uint32_t)size) to_read = (uint32_t)size;

    /* Walk to the starting cluster */
    cluster      = priv->first_cluster;
    start_cl_idx = offset / cluster_sz;

    for (i = 0; i < start_cl_idx; i++) {
        cluster = fat16_next_cluster(vol, cluster);
        if (cluster < 2U || cluster >= FAT16_EOC_MIN) return 0;
    }

    tmp = (uint8_t*)kmalloc(cluster_sz);
    if (!tmp) return -1;

    written = 0;
    {
        uint32_t cl_idx = start_cl_idx;

        while (written < to_read && cluster >= 2U && cluster < FAT16_EOC_MIN) {
            uint32_t cl_off = (cl_idx == start_cl_idx)
                              ? (offset % cluster_sz) : 0U;
            uint32_t avail  = cluster_sz - cl_off;
            uint32_t n      = to_read - written;
            uint32_t k;

            if (n > avail) n = avail;

            if (fat16_read_cluster(vol, cluster, tmp) < 0) break;

            for (k = 0; k < n; k++)
                buf[written + k] = tmp[cl_off + k];

            written += n;
            cl_idx++;
            cluster = fat16_next_cluster(vol, cluster);
        }
    }

    kfree(tmp);
    return (int)written;
}

/* ============================================================
 *  Write helpers: 8.3 name encoder and free-slot finder
 * ============================================================ */

/* Encode a VFS filename to 8.3 FAT uppercase format (space-padded).
   Names longer than 8 (stem) or 3 (ext) chars are truncated silently.
   Returns 0 on success, -1 on empty/invalid name. */
static int fat16_encode_83(const char* name, uint8_t* out8, uint8_t* outx) {
    unsigned int i;
    unsigned int j;
    const char*  dot      = 0;
    unsigned int stem_len;

    /* Find last dot not at position 0 */
    for (i = 0; name[i]; i++) {
        if (name[i] == '.' && i > 0U) dot = name + i;
    }

    for (i = 0; i < 8U; i++) out8[i] = ' ';
    for (i = 0; i < 3U; i++) outx[i] = ' ';

    stem_len = dot ? (unsigned int)(dot - name) : str_length(name);
    if (stem_len > 8U) stem_len = 8U;
    for (i = 0; i < stem_len; i++) {
        uint8_t c = (uint8_t)name[i];
        if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
        out8[i] = c;
    }

    if (dot && dot[1]) {
        const char* ext = dot + 1;
        for (j = 0; ext[j] && j < 3U; j++) {
            uint8_t c = (uint8_t)ext[j];
            if (c >= 'a' && c <= 'z') c = (uint8_t)(c - 'a' + 'A');
            outx[j] = c;
        }
    }

    if (out8[0] == ' ' || out8[0] == 0x00 || (uint8_t)out8[0] == 0xE5U) return -1;
    return 0;
}

typedef struct {
    uint32_t lba;
    uint32_t idx;
    int      found;
} fat16_free_slot_t;

/* Find a free directory entry (deleted 0xE5 or never-used 0x00).
   For subdirectory cluster chains, extends by one cluster if full.
   Returns 0 on success filling *out, -1 if no slot available. */
static int fat16_find_free_slot(fat16_vol_t* vol, uint16_t first_cluster,
                                 fat16_free_slot_t* out) {
    const uint32_t bps = (uint32_t)vol->bytes_per_sector;
    const uint32_t eps = bps / sizeof(fat16_dirent_t);
    uint8_t  sec[512];
    uint32_t i;

    out->found = 0;

    if (first_cluster == 0) {
        uint32_t total_s = ((uint32_t)vol->root_entry_count * 32U + bps - 1U) / bps;
        uint32_t s;

        for (s = 0; s < total_s; s++) {
            fat16_dirent_t* ent;
            uint32_t lba = vol->root_lba + s;
            if (fat16_read_sector(vol, lba, sec) < 0) continue;
            ent = (fat16_dirent_t*)(void*)sec;
            for (i = 0; i < eps; i++) {
                if (ent[i].name[0] == 0x00 || (uint8_t)ent[i].name[0] == 0xE5U) {
                    out->lba   = lba;
                    out->idx   = i;
                    out->found = 1;
                    return 0;
                }
            }
        }
        return -1; /* root directory full */
    } else {
        uint8_t* cbuf = (uint8_t*)kmalloc(vol->cluster_size);
        uint16_t cluster = first_cluster;
        uint16_t prev    = 0;

        if (!cbuf) return -1;

        while (cluster >= 2U && cluster < FAT16_EOC_MIN) {
            fat16_dirent_t* ent;
            uint32_t total_e = vol->cluster_size / sizeof(fat16_dirent_t);
            uint32_t base    = vol->data_lba
                               + (uint32_t)(cluster - 2U) * vol->sectors_per_cluster;

            if (fat16_read_cluster(vol, cluster, cbuf) == 0) {
                ent = (fat16_dirent_t*)(void*)cbuf;
                for (i = 0; i < total_e; i++) {
                    if (ent[i].name[0] == 0x00 || (uint8_t)ent[i].name[0] == 0xE5U) {
                        out->lba   = base + i / eps;
                        out->idx   = i % eps;
                        out->found = 1;
                        kfree(cbuf);
                        return 0;
                    }
                }
            }
            prev    = cluster;
            cluster = fat16_next_cluster(vol, cluster);
        }

        /* Directory full — extend by one cluster */
        if (prev >= 2U) {
            uint16_t new_cl = fat16_alloc_cluster(vol);
            if (new_cl) {
                uint32_t z;
                uint32_t base;
                for (z = 0; z < vol->cluster_size; z++) cbuf[z] = 0;
                if (fat16_write_cluster(vol, new_cl, cbuf) == 0) {
                    fat16_write_fat_entry(vol, prev, new_cl);
                    base = vol->data_lba
                           + (uint32_t)(new_cl - 2U) * vol->sectors_per_cluster;
                    out->lba   = base;
                    out->idx   = 0;
                    out->found = 1;
                    kfree(cbuf);
                    return 0;
                }
                fat16_write_fat_entry(vol, new_cl, 0x0000U); /* rollback */
            }
        }

        kfree(cbuf);
        return -1;
    }
}

/* ============================================================
 *  VFS ops: file write
 * ============================================================ */

static int fat16_file_write(vfs_node_t* node, unsigned int offset,
                             unsigned int size, const unsigned char* buf) {
    fat16_priv_t* priv         = (fat16_priv_t*)node->impl;
    fat16_vol_t*  vol          = priv->vol;
    uint32_t      cluster_sz   = vol->cluster_size;
    uint32_t      end          = offset + (uint32_t)size;
    uint32_t      clusters_needed;
    uint32_t      target_cl_idx;
    uint32_t      cl_idx;
    uint16_t      cluster;
    uint32_t      written;
    uint8_t*      tmp;

    if (size == 0) return 0;
    if (cluster_sz == 0) return -1;

    tmp = (uint8_t*)kmalloc(cluster_sz);
    if (!tmp) return -1;

    /* ---- Ensure file has at least one cluster ---- */
    if (priv->first_cluster < 2U) {
        uint16_t new_cl;
        uint32_t z;

        new_cl = fat16_alloc_cluster(vol);
        if (!new_cl) { kfree(tmp); return -1; }
        for (z = 0; z < cluster_sz; z++) tmp[z] = 0;
        if (fat16_write_cluster(vol, new_cl, tmp) < 0) {
            fat16_write_fat_entry(vol, new_cl, 0x0000U);
            kfree(tmp);
            return -1;
        }
        priv->first_cluster = new_cl;
        fat16_update_dirent(vol, priv->dirent_lba, priv->dirent_idx,
                            new_cl, priv->size);
    }

    /* ---- Extend cluster chain to cover [0 .. end) ---- */
    clusters_needed = (end + cluster_sz - 1U) / cluster_sz;
    {
        uint16_t cur   = priv->first_cluster;
        uint32_t count = 1;

        while (count < clusters_needed) {
            uint16_t nxt = fat16_next_cluster(vol, cur);
            if (nxt < 2U || nxt >= FAT16_EOC_MIN) {
                uint16_t new_cl;
                uint32_t z;

                new_cl = fat16_alloc_cluster(vol);
                if (!new_cl) { kfree(tmp); return -1; }
                for (z = 0; z < cluster_sz; z++) tmp[z] = 0;
                fat16_write_cluster(vol, new_cl, tmp);
                fat16_write_fat_entry(vol, cur, new_cl); /* link cur → new_cl */
                cur = new_cl;
            } else {
                cur = nxt;
            }
            count++;
        }
    }

    /* ---- Navigate to the starting cluster ---- */
    target_cl_idx = offset / cluster_sz;
    cluster       = priv->first_cluster;
    for (cl_idx = 0; cl_idx < target_cl_idx; cl_idx++) {
        cluster = fat16_next_cluster(vol, cluster);
        if (cluster < 2U || cluster >= FAT16_EOC_MIN) { kfree(tmp); return -1; }
    }

    /* ---- Write data spanning cluster boundaries ---- */
    written = 0;
    while (written < (uint32_t)size && cluster >= 2U && cluster < FAT16_EOC_MIN) {
        uint32_t cl_off = (cl_idx == target_cl_idx) ? (offset % cluster_sz) : 0U;
        uint32_t avail  = cluster_sz - cl_off;
        uint32_t n      = (uint32_t)size - written;
        uint32_t k;

        if (n > avail) n = avail;

        /* Read-modify-write for partial cluster fills */
        if (cl_off > 0U || n < cluster_sz) {
            if (fat16_read_cluster(vol, cluster, tmp) < 0) break;
        }
        for (k = 0; k < n; k++)
            tmp[cl_off + k] = buf[written + k];
        if (fat16_write_cluster(vol, cluster, tmp) < 0) break;

        written += n;
        cl_idx++;
        cluster = fat16_next_cluster(vol, cluster);
    }

    kfree(tmp);

    /* ---- Update file size and dirent ---- */
    if (end > (uint32_t)priv->size) {
        priv->size = end;
        node->size = end;
    }
    fat16_update_dirent(vol, priv->dirent_lba, priv->dirent_idx,
                        priv->first_cluster, priv->size);
    return (int)written;
}

/* ============================================================
 *  VFS ops: directory create / unlink
 * ============================================================ */

static vfs_node_t* fat16_dir_create(vfs_node_t* dir_node, const char* name,
                                     unsigned int flags) {
    fat16_priv_t*           dp      = (fat16_priv_t*)dir_node->impl;
    fat16_vol_t*            vol     = dp->vol;
    uint8_t                 n8[8];
    uint8_t                 nx[3];
    fat16_free_slot_t       slot;
    fat16_finddir_loc_ctx_t chk;
    uint8_t                 sec[512];
    fat16_dirent_t*         ent;
    uint16_t                first_cl = 0;
    fat16_node_t*           fn;
    unsigned int            i;

    if (fat16_encode_83(name, n8, nx) < 0) return 0;

    /* Reject duplicate names */
    chk.target = name;
    chk.found  = 0;
    fat16_walk_dir_loc(vol, dp->first_cluster, fat16_finddir_loc_cb, &chk);
    if (chk.found) return 0;

    /* For a new directory: allocate a cluster, write "." and ".." */
    if (flags & VFS_FLAG_DIR) {
        uint8_t* cbuf = (uint8_t*)kmalloc(vol->cluster_size);
        fat16_dirent_t* de;
        uint32_t z;

        if (!cbuf) return 0;
        first_cl = fat16_alloc_cluster(vol);
        if (!first_cl) { kfree(cbuf); return 0; }

        for (z = 0; z < vol->cluster_size; z++) cbuf[z] = 0;
        de = (fat16_dirent_t*)(void*)cbuf;

        /* "." */
        for (i = 0; i < 8U; i++) de[0].name[i] = ' ';
        for (i = 0; i < 3U; i++) de[0].ext[i]  = ' ';
        de[0].name[0]       = '.';
        de[0].attr          = FAT_ATTR_DIR;
        de[0].first_cluster = first_cl;

        /* ".." */
        for (i = 0; i < 8U; i++) de[1].name[i] = ' ';
        for (i = 0; i < 3U; i++) de[1].ext[i]  = ' ';
        de[1].name[0]       = '.';
        de[1].name[1]       = '.';
        de[1].attr          = FAT_ATTR_DIR;
        de[1].first_cluster = dp->first_cluster;

        fat16_write_cluster(vol, first_cl, cbuf);
        kfree(cbuf);
    }

    /* Find a free slot in the parent directory */
    if (fat16_find_free_slot(vol, dp->first_cluster, &slot) < 0) {
        if (first_cl) fat16_write_fat_entry(vol, first_cl, 0x0000U);
        return 0;
    }

    /* Write the directory entry */
    if (fat16_read_sector(vol, slot.lba, sec) < 0) {
        if (first_cl) fat16_write_fat_entry(vol, first_cl, 0x0000U);
        return 0;
    }
    ent = (fat16_dirent_t*)(void*)sec;
    for (i = 0; i < 8U; i++) ent[slot.idx].name[i] = n8[i];
    for (i = 0; i < 3U; i++) ent[slot.idx].ext[i]  = nx[i];
    ent[slot.idx].attr              = (flags & VFS_FLAG_DIR)
                                      ? FAT_ATTR_DIR : FAT_ATTR_ARCHIVE;
    ent[slot.idx].nt_reserved       = 0;
    ent[slot.idx].create_time_tenth = 0;
    ent[slot.idx].create_time       = 0;
    ent[slot.idx].create_date       = 0;
    ent[slot.idx].access_date       = 0;
    ent[slot.idx].first_cluster_hi  = 0;
    ent[slot.idx].mod_time          = 0;
    ent[slot.idx].mod_date          = 0;
    ent[slot.idx].first_cluster     = first_cl;
    ent[slot.idx].size              = 0;

    if (fat16_write_sector(vol, slot.lba, sec) < 0) {
        if (first_cl) fat16_write_fat_entry(vol, first_cl, 0x0000U);
        return 0;
    }

    /* Build and return a VFS node for the new entry */
    fn = (fat16_node_t*)kmalloc(sizeof(fat16_node_t));
    if (!fn) return 0;

    fn->priv.vol           = vol;
    fn->priv.first_cluster = first_cl;
    fn->priv.size          = 0;
    fn->priv.dirent_lba    = slot.lba;
    fn->priv.dirent_idx    = slot.idx;

    for (i = 0; name[i] && i < VFS_NAME_MAX - 1U; i++)
        fn->node.name[i] = name[i];
    fn->node.name[i] = '\0';
    fn->node.impl       = &fn->priv;
    fn->node.mountpoint = 0;
    fn->node.size       = 0;

    if (flags & VFS_FLAG_DIR) {
        fn->node.flags = VFS_FLAG_DIR  | VFS_FLAG_DYNAMIC;
        fn->node.ops   = &fat16_dir_ops;
    } else {
        fn->node.flags = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
        fn->node.ops   = &fat16_file_ops;
    }

    return &fn->node;
}

static int fat16_dir_unlink(vfs_node_t* dir_node, const char* name) {
    fat16_priv_t*           dp  = (fat16_priv_t*)dir_node->impl;
    fat16_vol_t*            vol = dp->vol;
    fat16_finddir_loc_ctx_t ctx;
    uint8_t                 sec[512];
    fat16_dirent_t*         ent;

    ctx.target = name;
    ctx.found  = 0;
    fat16_walk_dir_loc(vol, dp->first_cluster, fat16_finddir_loc_cb, &ctx);
    if (!ctx.found) return -1;

    /* Mark entry as deleted */
    if (fat16_read_sector(vol, ctx.dirent_lba, sec) < 0) return -1;
    ent = (fat16_dirent_t*)(void*)sec;
    ent[ctx.dirent_idx].name[0] = (uint8_t)0xE5U;
    if (fat16_write_sector(vol, ctx.dirent_lba, sec) < 0) return -1;

    /* Release cluster chain */
    if (ctx.result.first_cluster >= 2U)
        fat16_free_cluster_chain(vol, ctx.result.first_cluster);

    return 0;
}

/* ============================================================
 *  fat16_mount
 * ============================================================ */

vfs_node_t* fat16_mount(int blkdev_idx) {
    uint8_t      sec[512];
    fat16_bpb_t* bpb;
    fat16_vol_t* vol = 0;
    fat16_priv_t* rp;
    vfs_node_t*   rn;
    uint32_t fat_lba, root_lba, data_lba;
    int i;

    /* Find a free volume slot */
    for (i = 0; i < FAT16_MAX_VOLUMES; i++) {
        if (!vol_pool[i].used) { vol = &vol_pool[i]; break; }
    }
    if (!vol) return 0;

    /* Read sector 0 (BPB) */
    if (blkdev_read(blkdev_idx, 0U, 1U, sec) != 1) return 0;

    /* MBR boot signature */
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;

    bpb = (fat16_bpb_t*)(void*)sec;

    /* ---- Basic FAT16 validation ---- */
    if (bpb->bytes_per_sector != 512) return 0;  /* only 512-byte sectors */
    if (bpb->sectors_per_cluster == 0) return 0;
    if (bpb->fat_count == 0)           return 0;
    if (bpb->sectors_per_fat == 0)     return 0;  /* FAT32 has 0 here */
    if (bpb->root_entry_count == 0)    return 0;  /* FAT32 has 0 here */
    if (bpb->reserved_sectors == 0)    return 0;

    /* ---- Derived layout ---- */
    fat_lba  = (uint32_t)bpb->reserved_sectors;
    root_lba = fat_lba + (uint32_t)bpb->fat_count * (uint32_t)bpb->sectors_per_fat;
    data_lba = root_lba + ((uint32_t)bpb->root_entry_count * 32U
                           + (uint32_t)bpb->bytes_per_sector - 1U)
                          / (uint32_t)bpb->bytes_per_sector;

    vol->blkdev_idx          = blkdev_idx;
    vol->bytes_per_sector    = bpb->bytes_per_sector;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->cluster_size        = (uint32_t)bpb->bytes_per_sector
                               * (uint32_t)bpb->sectors_per_cluster;
    vol->fat_lba             = fat_lba;
    vol->root_lba            = root_lba;
    vol->root_entry_count    = bpb->root_entry_count;
    vol->data_lba            = data_lba;
    vol->fat_count           = bpb->fat_count;
    vol->sectors_per_fat     = bpb->sectors_per_fat;
    vol->used                = 1;

    /* ---- Build static root node (NOT VFS_FLAG_DYNAMIC) ---- */
    rp = &root_priv_pool[i];
    rp->vol           = vol;
    rp->first_cluster = 0;     /* 0 = root directory sentinel */
    rp->size          = 0;
    rp->dirent_lba    = 0;     /* root has no parent dirent */
    rp->dirent_idx    = 0;

    rn = &root_node_pool[i];
    rn->name[0]    = '/';
    rn->name[1]    = '\0';
    rn->flags      = VFS_FLAG_DIR;  /* no VFS_FLAG_DYNAMIC */
    rn->size       = 0;
    rn->impl       = rp;
    rn->ops        = &fat16_dir_ops;
    rn->mountpoint = 0;

    return rn;
}
