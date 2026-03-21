#include "fat32.h"
#include "blkdev.h"
#include "heap.h"
#include "string.h"
#include <stdint.h>

/* ============================================================
 *  FAT32 on-disk structures
 * ============================================================ */

typedef struct __attribute__((packed)) {
    uint8_t  jmp[3];
    uint8_t  oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_entry_count;    /* 0 on FAT32 */
    uint16_t total_sectors_16;    /* 0 on FAT32 */
    uint8_t  media_type;
    uint16_t fat_size_16;         /* 0 on FAT32 */
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    /* FAT32 extended BPB (offset 36): */
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;        /* first cluster of root dir (usually 2) */
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];          /* "FAT32   " */
} fat32_bpb_t;

/* Standard 32-byte directory entry (same layout as FAT16,
   but first_cluster_hi is meaningful on FAT32). */
typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;   /* upper 16 bits of first cluster */
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t first_cluster_lo;   /* lower 16 bits of first cluster */
    uint32_t size;
} fat32_dirent_t;

/* LFN entry (identical layout to FAT16 LFN) */
typedef struct __attribute__((packed)) {
    uint8_t  order;       /* seq num; bit 6 (0x40) set on first entry */
    uint16_t name1[5];    /* UTF-16LE chars  1-5  */
    uint8_t  attr;        /* always 0x0F */
    uint8_t  type;        /* always 0x00 */
    uint8_t  checksum;
    uint16_t name2[6];    /* UTF-16LE chars  6-11 */
    uint16_t first_cl;    /* always 0x0000 */
    uint16_t name3[2];    /* UTF-16LE chars 12-13 */
} fat32_lfn_entry_t;

#define FAT_ATTR_READONLY32   0x01
#define FAT_ATTR_HIDDEN32     0x02
#define FAT_ATTR_SYSTEM32     0x04
#define FAT_ATTR_VOLID32      0x08
#define FAT_ATTR_DIR32        0x10
#define FAT_ATTR_ARCHIVE32    0x20
#define FAT_ATTR_LFN32        0x0F  /* long-filename marker */

#define FAT32_EOC_MIN         0x0FFFFFF8UL
#define FAT32_CLUSTER_MASK    0x0FFFFFFFUL

/* ============================================================
 *  Volume descriptor
 * ============================================================ */

typedef struct {
    int      blkdev_idx;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  fat_count;
    uint32_t cluster_size;        /* bytes per cluster */
    uint32_t fat_lba;             /* first sector of FAT1 */
    uint32_t data_lba;            /* first sector of cluster 2 */
    uint32_t root_cluster;
    int      used;
} fat32_vol_t;

static fat32_vol_t vol_pool[FAT32_MAX_VOLUMES];

/* Per-node private data */
typedef struct {
    fat32_vol_t* vol;
    uint32_t     first_cluster;
    uint32_t     size;
    uint32_t     dirent_lba;
    uint32_t     dirent_idx;
} fat32_priv_t;

/* Combined heap-allocated node + priv (vfs_node_t MUST be first). */
typedef struct {
    vfs_node_t   node;
    fat32_priv_t priv;
} fat32_node_t;

/* Root node pool – static, NOT VFS_FLAG_DYNAMIC. */
static vfs_node_t   root_node_pool[FAT32_MAX_VOLUMES];
static fat32_priv_t root_priv_pool[FAT32_MAX_VOLUMES];

/* ============================================================
 *  LFN accumulator
 * ============================================================ */

typedef struct {
    char buf[256];  /* assembled LFN (NUL-terminated) */
    int  valid;     /* 1 = buf holds a pending LFN */
} fat32_lfn_state_t;

static void lfn32_state_reset(fat32_lfn_state_t* st) {
    st->valid  = 0;
    st->buf[0] = '\0';
}

/* Extract up to 13 UTF-16LE chars from one LFN entry into dst[base..]. */
static void lfn32_extract_13(const fat32_lfn_entry_t* e, char* dst, int base) {
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
static void lfn32_state_feed(fat32_lfn_state_t* st, const fat32_lfn_entry_t* e) {
    int order = (int)(e->order & 0x3FU);
    int slot  = order - 1;
    int z;
    if (slot < 0 || slot >= 20) { st->valid = 0; return; }
    if (e->order & 0x40U) {
        /* First entry encountered (highest seq #): zero the buffer. */
        for (z = 0; z < 256; z++) st->buf[z] = '\0';
        st->valid = 1;
    }
    if (!st->valid) return;
    lfn32_extract_13(e, st->buf, slot * 13);
}

/* ============================================================
 *  Low-level I/O
 * ============================================================ */

static int fat32_read_sector(fat32_vol_t* vol, uint32_t lba, uint8_t* buf) {
    return (blkdev_read(vol->blkdev_idx, lba, 1, buf) == 1) ? 0 : -1;
}

/* Return the next cluster in the chain (already masked to 28 bits),
   or a value >= FAT32_EOC_MIN on error or end-of-chain. */
static uint32_t fat32_next_cluster(fat32_vol_t* vol, uint32_t cluster) {
    uint8_t  sec[512];
    uint32_t byte_off = cluster * 4U;
    uint32_t fat_sec  = vol->fat_lba + byte_off / (uint32_t)vol->bytes_per_sector;
    uint32_t sec_off  = byte_off % (uint32_t)vol->bytes_per_sector;
    uint32_t val;

    if (fat32_read_sector(vol, fat_sec, sec) < 0) return FAT32_EOC_MIN;

    val = (uint32_t)sec[sec_off]
        | ((uint32_t)sec[sec_off + 1U] << 8)
        | ((uint32_t)sec[sec_off + 2U] << 16)
        | ((uint32_t)sec[sec_off + 3U] << 24);
    return val & FAT32_CLUSTER_MASK;
}

static int fat32_read_cluster(fat32_vol_t* vol, uint32_t cluster, uint8_t* buf) {
    uint32_t lba = vol->data_lba + (cluster - 2U) * (uint32_t)vol->sectors_per_cluster;
    uint32_t s;
    for (s = 0; s < (uint32_t)vol->sectors_per_cluster; s++) {
        if (fat32_read_sector(vol, lba + s,
                              buf + s * (uint32_t)vol->bytes_per_sector) < 0)
            return -1;
    }
    return 0;
}

/* ============================================================
 *  Low-level write helpers
 * ============================================================ */

static int fat32_write_sector(fat32_vol_t* vol, uint32_t lba, const uint8_t* buf) {
    return (blkdev_write(vol->blkdev_idx, lba, 1, buf) == 1) ? 0 : -1;
}

static int fat32_write_cluster(fat32_vol_t* vol, uint32_t cluster, const uint8_t* buf) {
    uint32_t lba = vol->data_lba + (cluster - 2U) * (uint32_t)vol->sectors_per_cluster;
    uint32_t s;
    for (s = 0; s < (uint32_t)vol->sectors_per_cluster; s++) {
        if (fat32_write_sector(vol, lba + s,
                               buf + s * (uint32_t)vol->bytes_per_sector) < 0)
            return -1;
    }
    return 0;
}

/* Write one FAT32 entry (28-bit value) into every FAT copy. */
static int fat32_write_fat_entry(fat32_vol_t* vol, uint32_t cluster, uint32_t val) {
    uint8_t  sec[512];
    uint32_t byte_off = cluster * 4U;
    uint32_t fat_sec  = byte_off / (uint32_t)vol->bytes_per_sector;
    uint32_t sec_off  = byte_off % (uint32_t)vol->bytes_per_sector;
    uint32_t copy;
    uint32_t sectors_per_fat;

    val &= FAT32_CLUSTER_MASK;
    if (sec_off + 3U >= (uint32_t)vol->bytes_per_sector) return -1;

    sectors_per_fat = (vol->data_lba - vol->fat_lba) / (uint32_t)vol->fat_count;
    for (copy = 0; copy < (uint32_t)vol->fat_count; copy++) {
        uint32_t lba = vol->fat_lba + copy * sectors_per_fat + fat_sec;
        if (fat32_read_sector(vol, lba, sec) < 0) return -1;
        sec[sec_off + 0U] = (uint8_t)(val & 0xFFU);
        sec[sec_off + 1U] = (uint8_t)((val >> 8) & 0xFFU);
        sec[sec_off + 2U] = (uint8_t)((val >> 16) & 0xFFU);
        sec[sec_off + 3U] = (uint8_t)((val >> 24) & 0x0FU); /* upper nibble reserved */
        if (fat32_write_sector(vol, lba, sec) < 0) return -1;
    }

    return 0;
}

/* Scan FAT1 for a free cluster and mark it EOC. Returns 0 on failure. */
static uint32_t fat32_alloc_cluster(fat32_vol_t* vol) {
    uint8_t  sec[512];
    uint32_t bps = (uint32_t)vol->bytes_per_sector;
    uint32_t entries_per_s = bps / 4U;
    uint32_t sectors_per_fat = (vol->data_lba - vol->fat_lba) / (uint32_t)vol->fat_count;
    uint32_t s, i;

    for (s = 0; s < sectors_per_fat; s++) {
        if (fat32_read_sector(vol, vol->fat_lba + s, sec) < 0) continue;
        for (i = 0; i < entries_per_s; i++) {
            uint32_t off = i * 4U;
            uint32_t entry = (uint32_t)sec[off]
                           | ((uint32_t)sec[off + 1U] << 8)
                           | ((uint32_t)sec[off + 2U] << 16)
                           | ((uint32_t)sec[off + 3U] << 24);
            entry &= FAT32_CLUSTER_MASK;
            if (entry == 0U) {
                uint32_t cluster = s * entries_per_s + i;
                if (cluster < 2U) continue;
                if (fat32_write_fat_entry(vol, cluster, 0x0FFFFFFFU) < 0) return 0;
                return cluster;
            }
        }
    }
    return 0;
}

static void fat32_free_cluster_chain(fat32_vol_t* vol, uint32_t first_cluster) {
    uint32_t cluster = first_cluster;
    while (cluster >= 2U && cluster < FAT32_EOC_MIN) {
        uint32_t next = fat32_next_cluster(vol, cluster);
        fat32_write_fat_entry(vol, cluster, 0U);
        cluster = next;
    }
}

static int fat32_update_dirent(fat32_vol_t* vol,
                               uint32_t dirent_lba, uint32_t dirent_idx,
                               uint32_t first_cluster, uint32_t file_size) {
    uint8_t         sec[512];
    fat32_dirent_t* ent;

    if (dirent_lba == 0) return -1;
    if (fat32_read_sector(vol, dirent_lba, sec) < 0) return -1;
    ent = (fat32_dirent_t*)(void*)sec;
    ent[dirent_idx].first_cluster_lo = (uint16_t)(first_cluster & 0xFFFFU);
    ent[dirent_idx].first_cluster_hi = (uint16_t)((first_cluster >> 16) & 0xFFFFU);
    ent[dirent_idx].size             = file_size;
    return fat32_write_sector(vol, dirent_lba, sec);
}

/* ============================================================
 *  Name formatting
 * ============================================================ */

static void fat32_format_name(const uint8_t* name8, const uint8_t* ext3, char* dst) {
    int nlen, elen, i, j = 0;
    for (nlen = 8; nlen > 0 && name8[nlen - 1] == ' '; nlen--) ;
    for (i = 0; i < nlen; i++) {
        uint8_t c = name8[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c - 'A' + 'a');
        dst[j++] = (char)c;
    }
    for (elen = 3; elen > 0 && ext3[elen - 1] == ' '; elen--) ;
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

static int fat32_name_match(const char* vfs_name,
                             const uint8_t* name8, const uint8_t* ext3) {
    char formatted[13];
    fat32_format_name(name8, ext3, formatted);
    return str_equals_ignore_case(vfs_name, formatted);
}

/* ============================================================
 *  Directory walker with LFN support
 *
 *  Calls cb(cookie, entry, lfn_or_null) for each valid 8.3 entry.
 *  lfn_or_null is non-NULL when Long File Name entries preceded the
 *  8.3 entry; it points to an ASCII representation of the full name.
 * ============================================================ */

typedef int (*fat32_dir_cb)(void* cookie, const fat32_dirent_t* e,
                            const char* lfn);

static int fat32_walk_dir(fat32_vol_t* vol, uint32_t first_cluster,
                          fat32_dir_cb cb, void* cookie) {
    uint8_t*          cbuf;
    uint32_t          cluster = first_cluster;
    fat32_lfn_state_t lfn;

    if (vol->cluster_size == 0) return 0;
    cbuf = (uint8_t*)kmalloc(vol->cluster_size);
    if (!cbuf) return 0;

    lfn32_state_reset(&lfn);

    while (cluster >= 2U && cluster < FAT32_EOC_MIN) {
        fat32_dirent_t* ent;
        uint32_t        eps = vol->cluster_size / sizeof(fat32_dirent_t);
        uint32_t        i;

        if (fat32_read_cluster(vol, cluster, cbuf) < 0) break;
        ent = (fat32_dirent_t*)(void*)cbuf;

        for (i = 0; i < eps; i++) {
            if (ent[i].name[0] == 0x00) { kfree(cbuf); return 0; }
            if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn32_state_reset(&lfn); continue; }
            if (ent[i].attr == FAT_ATTR_LFN32) {
                lfn32_state_feed(&lfn, (const fat32_lfn_entry_t*)(const void*)&ent[i]);
                continue;
            }
            if (ent[i].attr & FAT_ATTR_VOLID32) { lfn32_state_reset(&lfn); continue; }
            if (ent[i].name[0] == '.') { lfn32_state_reset(&lfn); continue; }
            {
                int r = cb(cookie, &ent[i], lfn.valid ? lfn.buf : (const char*)0);
                lfn32_state_reset(&lfn);
                if (r) { kfree(cbuf); return r; }
            }
        }
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(cbuf);
    return 0;
}

/* Located walker — also returns LBA + index within that sector. */

typedef int (*fat32_dir_cb_loc)(void* cookie, const fat32_dirent_t* e,
                                uint32_t lba, uint32_t idx, const char* lfn);

static int fat32_walk_dir_loc(fat32_vol_t* vol, uint32_t first_cluster,
                               fat32_dir_cb_loc cb, void* cookie) {
    const uint32_t    bps = (uint32_t)vol->bytes_per_sector;
    const uint32_t    eps = bps / sizeof(fat32_dirent_t);
    uint8_t*          cbuf;
    uint32_t          cluster = first_cluster;
    fat32_lfn_state_t lfn;

    if (vol->cluster_size == 0) return 0;
    cbuf = (uint8_t*)kmalloc(vol->cluster_size);
    if (!cbuf) return 0;

    lfn32_state_reset(&lfn);

    while (cluster >= 2U && cluster < FAT32_EOC_MIN) {
        fat32_dirent_t* ent;
        uint32_t        total_e  = vol->cluster_size / sizeof(fat32_dirent_t);
        uint32_t        base_lba = vol->data_lba
                                   + (cluster - 2U) * (uint32_t)vol->sectors_per_cluster;
        uint32_t        i;

        if (fat32_read_cluster(vol, cluster, cbuf) < 0) break;
        ent = (fat32_dirent_t*)(void*)cbuf;

        for (i = 0; i < total_e; i++) {
            uint32_t sec_i = i / eps;
            uint32_t ent_i = i % eps;
            uint32_t lba   = base_lba + sec_i;

            if (ent[i].name[0] == 0x00) { kfree(cbuf); return 0; }
            if ((uint8_t)ent[i].name[0] == 0xE5U) { lfn32_state_reset(&lfn); continue; }
            if (ent[i].attr == FAT_ATTR_LFN32) {
                lfn32_state_feed(&lfn, (const fat32_lfn_entry_t*)(const void*)&ent[i]);
                continue;
            }
            if (ent[i].attr & FAT_ATTR_VOLID32) { lfn32_state_reset(&lfn); continue; }
            if (ent[i].name[0] == '.') { lfn32_state_reset(&lfn); continue; }
            {
                int r = cb(cookie, &ent[i], lba, ent_i,
                           lfn.valid ? lfn.buf : (const char*)0);
                lfn32_state_reset(&lfn);
                if (r) { kfree(cbuf); return r; }
            }
        }
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(cbuf);
    return 0;
}

/* ============================================================
 *  Walker callbacks
 * ============================================================ */

typedef struct {
    unsigned int idx;
    unsigned int cur;
    char*        out;
    unsigned int out_max;
    int          found;
} fat32_readdir_ctx_t;

static int fat32_readdir_cb(void* cookie, const fat32_dirent_t* e,
                             const char* lfn) {
    fat32_readdir_ctx_t* ctx = (fat32_readdir_ctx_t*)cookie;

    if (ctx->cur == ctx->idx) {
        if (lfn && lfn[0]) {
            unsigned int i = 0;
            while (lfn[i] && i < ctx->out_max - 1U) { ctx->out[i] = lfn[i]; i++; }
            ctx->out[i] = '\0';
        } else {
            fat32_format_name(e->name, e->ext, ctx->out);
        }
        ctx->found = 1;
        return 1;   /* stop */
    }
    ctx->cur++;
    return 0;
}

typedef struct {
    const char*    target;
    fat32_dirent_t result;
    uint32_t       dirent_lba;
    uint32_t       dirent_idx;
    int            found;
} fat32_finddir_loc_ctx_t;

static int fat32_finddir_loc_cb(void* cookie, const fat32_dirent_t* e,
                                 uint32_t lba, uint32_t idx, const char* lfn) {
    fat32_finddir_loc_ctx_t* ctx = (fat32_finddir_loc_ctx_t*)cookie;
    int match = 0;
    if (lfn && lfn[0]) match = str_equals_ignore_case(ctx->target, lfn);
    if (!match)        match = fat32_name_match(ctx->target, e->name, e->ext);
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
 *  VFS vtable declarations
 * ============================================================ */

static int         fat32_dir_readdir(vfs_node_t*, unsigned int, char*, unsigned int);
static vfs_node_t* fat32_dir_finddir(vfs_node_t*, const char*);
static int         fat32_file_read  (vfs_node_t*, unsigned int, unsigned int, unsigned char*);
static int         fat32_file_write (vfs_node_t*, unsigned int, unsigned int, const unsigned char*);
static vfs_node_t* fat32_dir_create (vfs_node_t*, const char*, unsigned int);
static int         fat32_dir_unlink (vfs_node_t*, const char*);

static vfs_ops_t fat32_file_ops = {
    .read    = fat32_file_read,
    .write   = fat32_file_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static vfs_ops_t fat32_dir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = fat32_dir_readdir,
    .finddir = fat32_dir_finddir,
    .open    = 0,
    .close   = 0,
    .create  = fat32_dir_create,
    .unlink  = fat32_dir_unlink,
};

/* ============================================================
 *  VFS ops: directory
 * ============================================================ */

static int fat32_dir_readdir(vfs_node_t* node, unsigned int index,
                              char* name_out, unsigned int name_max) {
    fat32_priv_t*       priv = (fat32_priv_t*)node->impl;
    fat32_readdir_ctx_t ctx;

    ctx.idx     = index;
    ctx.cur     = 0;
    ctx.out     = name_out;
    ctx.out_max = name_max;
    ctx.found   = 0;

    fat32_walk_dir(priv->vol, priv->first_cluster, fat32_readdir_cb, &ctx);
    return ctx.found ? 0 : -1;
}

static vfs_node_t* fat32_dir_finddir(vfs_node_t* node, const char* name) {
    fat32_priv_t*           priv = (fat32_priv_t*)node->impl;
    fat32_finddir_loc_ctx_t ctx;
    fat32_node_t*           fn;
    unsigned int            i;
    uint32_t                fc;

    ctx.target = name;
    ctx.found  = 0;

    fat32_walk_dir_loc(priv->vol, priv->first_cluster, fat32_finddir_loc_cb, &ctx);
    if (!ctx.found) return 0;

    fn = (fat32_node_t*)kmalloc(sizeof(fat32_node_t));
    if (!fn) return 0;

    fc = ((uint32_t)ctx.result.first_cluster_hi << 16) | ctx.result.first_cluster_lo;

    fn->priv.vol           = priv->vol;
    fn->priv.first_cluster = fc;
    fn->priv.size          = ctx.result.size;
    fn->priv.dirent_lba    = ctx.dirent_lba;
    fn->priv.dirent_idx    = ctx.dirent_idx;

    for (i = 0; name[i] && i < VFS_NAME_MAX - 1U; i++)
        fn->node.name[i] = name[i];
    fn->node.name[i]    = '\0';
    fn->node.impl       = &fn->priv;
    fn->node.mountpoint = 0;
    fn->node.ref_count  = 0;

    if (ctx.result.attr & FAT_ATTR_DIR32) {
        fn->node.flags = VFS_FLAG_DIR  | VFS_FLAG_DYNAMIC;
        fn->node.size  = 0;
        fn->node.ops   = &fat32_dir_ops;
    } else {
        fn->node.flags = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
        fn->node.size  = ctx.result.size;
        fn->node.ops   = &fat32_file_ops;
    }

    return &fn->node;
}

/* ============================================================
 *  VFS ops: file read
 * ============================================================ */

static int fat32_file_read(vfs_node_t* node, unsigned int offset,
                           unsigned int size, unsigned char* buf) {
    fat32_priv_t* priv       = (fat32_priv_t*)node->impl;
    fat32_vol_t*  vol        = priv->vol;
    uint32_t      file_size  = priv->size;
    uint32_t      cluster_sz = vol->cluster_size;
    uint32_t      to_read, written, start_cl_idx, cl_idx;
    uint32_t      cluster;
    uint8_t*      tmp;

    if (offset >= file_size || size == 0) return 0;

    to_read = file_size - offset;
    if (to_read > (uint32_t)size) to_read = (uint32_t)size;

    /* Walk to starting cluster */
    cluster      = priv->first_cluster;
    start_cl_idx = offset / cluster_sz;

    for (cl_idx = 0; cl_idx < start_cl_idx; cl_idx++) {
        cluster = fat32_next_cluster(vol, cluster);
        if (cluster < 2U || cluster >= FAT32_EOC_MIN) return 0;
    }

    tmp = (uint8_t*)kmalloc(cluster_sz);
    if (!tmp) return -1;

    written = 0;
    cl_idx  = start_cl_idx;
    while (written < to_read && cluster >= 2U && cluster < FAT32_EOC_MIN) {
        uint32_t cl_off = (cl_idx == start_cl_idx) ? (offset % cluster_sz) : 0U;
        uint32_t avail  = cluster_sz - cl_off;
        uint32_t n      = to_read - written;
        uint32_t k;

        if (n > avail) n = avail;
        if (fat32_read_cluster(vol, cluster, tmp) < 0) break;
        for (k = 0; k < n; k++) buf[written + k] = tmp[cl_off + k];
        written += n;
        cl_idx++;
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(tmp);
    return (int)written;
}

/* ============================================================
 *  Write helpers: 8.3 encoder and free-slot finder
 * ============================================================ */

static int fat32_encode_83(const char* name, uint8_t* out8, uint8_t* outx) {
    unsigned int i;
    unsigned int j;
    const char*  dot = 0;
    unsigned int stem_len;

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
} fat32_free_slot_t;

/* Find free slot in directory; extend by one cluster when full. */
static int fat32_find_free_slot(fat32_vol_t* vol, uint32_t first_cluster,
                                fat32_free_slot_t* out) {
    const uint32_t bps = (uint32_t)vol->bytes_per_sector;
    const uint32_t eps = bps / sizeof(fat32_dirent_t);
    uint8_t* cbuf;
    uint32_t cluster;
    uint32_t prev = 0;
    uint32_t i;

    if (!out) return -1;
    out->found = 0;

    cluster = (first_cluster >= 2U) ? first_cluster : vol->root_cluster;
    if (cluster < 2U || vol->cluster_size == 0) return -1;

    cbuf = (uint8_t*)kmalloc(vol->cluster_size);
    if (!cbuf) return -1;

    while (cluster >= 2U && cluster < FAT32_EOC_MIN) {
        fat32_dirent_t* ent;
        uint32_t total_e = vol->cluster_size / sizeof(fat32_dirent_t);
        uint32_t base = vol->data_lba + (cluster - 2U) * (uint32_t)vol->sectors_per_cluster;

        if (fat32_read_cluster(vol, cluster, cbuf) == 0) {
            ent = (fat32_dirent_t*)(void*)cbuf;
            for (i = 0; i < total_e; i++) {
                if (ent[i].name[0] == 0x00 || (uint8_t)ent[i].name[0] == 0xE5U) {
                    out->lba = base + i / eps;
                    out->idx = i % eps;
                    out->found = 1;
                    kfree(cbuf);
                    return 0;
                }
            }
        }

        prev = cluster;
        cluster = fat32_next_cluster(vol, cluster);
    }

    if (prev >= 2U) {
        uint32_t new_cl = fat32_alloc_cluster(vol);
        if (new_cl) {
            uint32_t z;
            uint32_t base;
            for (z = 0; z < vol->cluster_size; z++) cbuf[z] = 0;
            if (fat32_write_cluster(vol, new_cl, cbuf) == 0) {
                fat32_write_fat_entry(vol, prev, new_cl);
                base = vol->data_lba + (new_cl - 2U) * (uint32_t)vol->sectors_per_cluster;
                out->lba = base;
                out->idx = 0;
                out->found = 1;
                kfree(cbuf);
                return 0;
            }
            fat32_write_fat_entry(vol, new_cl, 0U);
        }
    }

    kfree(cbuf);
    return -1;
}

/* ============================================================
 *  VFS ops: file write
 * ============================================================ */

static int fat32_file_write(vfs_node_t* node, unsigned int offset,
                            unsigned int size, const unsigned char* buf) {
    fat32_priv_t* priv = (fat32_priv_t*)node->impl;
    fat32_vol_t*  vol  = priv->vol;
    uint32_t      cluster_sz = vol->cluster_size;
    uint32_t      end = offset + (uint32_t)size;
    uint32_t      clusters_needed;
    uint32_t      target_cl_idx;
    uint32_t      cl_idx;
    uint32_t      cluster;
    uint32_t      written;
    uint8_t*      tmp;

    if (size == 0) return 0;
    if (cluster_sz == 0) return -1;

    tmp = (uint8_t*)kmalloc(cluster_sz);
    if (!tmp) return -1;

    if (priv->first_cluster < 2U) {
        uint32_t new_cl;
        uint32_t z;
        new_cl = fat32_alloc_cluster(vol);
        if (!new_cl) { kfree(tmp); return -1; }
        for (z = 0; z < cluster_sz; z++) tmp[z] = 0;
        if (fat32_write_cluster(vol, new_cl, tmp) < 0) {
            fat32_write_fat_entry(vol, new_cl, 0U);
            kfree(tmp);
            return -1;
        }
        priv->first_cluster = new_cl;
        if (priv->dirent_lba)
            fat32_update_dirent(vol, priv->dirent_lba, priv->dirent_idx,
                                new_cl, priv->size);
    }

    clusters_needed = (end + cluster_sz - 1U) / cluster_sz;
    {
        uint32_t cur = priv->first_cluster;
        uint32_t count = 1;
        while (count < clusters_needed) {
            uint32_t nxt = fat32_next_cluster(vol, cur);
            if (nxt < 2U || nxt >= FAT32_EOC_MIN) {
                uint32_t new_cl;
                uint32_t z;
                new_cl = fat32_alloc_cluster(vol);
                if (!new_cl) { kfree(tmp); return -1; }
                for (z = 0; z < cluster_sz; z++) tmp[z] = 0;
                fat32_write_cluster(vol, new_cl, tmp);
                fat32_write_fat_entry(vol, cur, new_cl);
                cur = new_cl;
            } else {
                cur = nxt;
            }
            count++;
        }
    }

    target_cl_idx = offset / cluster_sz;
    cluster = priv->first_cluster;
    for (cl_idx = 0; cl_idx < target_cl_idx; cl_idx++) {
        cluster = fat32_next_cluster(vol, cluster);
        if (cluster < 2U || cluster >= FAT32_EOC_MIN) { kfree(tmp); return -1; }
    }

    written = 0;
    while (written < (uint32_t)size && cluster >= 2U && cluster < FAT32_EOC_MIN) {
        uint32_t cl_off = (cl_idx == target_cl_idx) ? (offset % cluster_sz) : 0U;
        uint32_t avail  = cluster_sz - cl_off;
        uint32_t n      = (uint32_t)size - written;
        uint32_t k;

        if (n > avail) n = avail;
        if (cl_off > 0U || n < cluster_sz) {
            if (fat32_read_cluster(vol, cluster, tmp) < 0) break;
        }
        for (k = 0; k < n; k++) tmp[cl_off + k] = buf[written + k];
        if (fat32_write_cluster(vol, cluster, tmp) < 0) break;

        written += n;
        cl_idx++;
        cluster = fat32_next_cluster(vol, cluster);
    }

    kfree(tmp);

    if (end > (uint32_t)priv->size) {
        priv->size = end;
        node->size = end;
    }

    if (priv->dirent_lba)
        fat32_update_dirent(vol, priv->dirent_lba, priv->dirent_idx,
                            priv->first_cluster, priv->size);
    return (int)written;
}

/* ============================================================
 *  VFS ops: directory create / unlink
 * ============================================================ */

static vfs_node_t* fat32_dir_create(vfs_node_t* dir_node, const char* name,
                                    unsigned int flags) {
    fat32_priv_t*           dp  = (fat32_priv_t*)dir_node->impl;
    fat32_vol_t*            vol = dp->vol;
    uint8_t                 n8[8];
    uint8_t                 nx[3];
    fat32_free_slot_t       slot;
    fat32_finddir_loc_ctx_t chk;
    uint8_t                 sec[512];
    fat32_dirent_t*         ent;
    uint32_t                first_cl = 0;
    fat32_node_t*           fn;
    uint32_t                parent_cl;
    unsigned int            i;

    if (fat32_encode_83(name, n8, nx) < 0) return 0;

    chk.target = name;
    chk.found = 0;
    fat32_walk_dir_loc(vol, dp->first_cluster, fat32_finddir_loc_cb, &chk);
    if (chk.found) return 0;

    if (flags & VFS_FLAG_DIR) {
        uint8_t* cbuf = (uint8_t*)kmalloc(vol->cluster_size);
        fat32_dirent_t* de;
        uint32_t z;

        if (!cbuf) return 0;
        first_cl = fat32_alloc_cluster(vol);
        if (!first_cl) { kfree(cbuf); return 0; }

        for (z = 0; z < vol->cluster_size; z++) cbuf[z] = 0;
        de = (fat32_dirent_t*)(void*)cbuf;

        for (i = 0; i < 8U; i++) de[0].name[i] = ' ';
        for (i = 0; i < 3U; i++) de[0].ext[i]  = ' ';
        de[0].name[0] = '.';
        de[0].attr = FAT_ATTR_DIR32;
        de[0].first_cluster_lo = (uint16_t)(first_cl & 0xFFFFU);
        de[0].first_cluster_hi = (uint16_t)((first_cl >> 16) & 0xFFFFU);

        parent_cl = (dp->first_cluster >= 2U) ? dp->first_cluster : vol->root_cluster;
        for (i = 0; i < 8U; i++) de[1].name[i] = ' ';
        for (i = 0; i < 3U; i++) de[1].ext[i]  = ' ';
        de[1].name[0] = '.';
        de[1].name[1] = '.';
        de[1].attr = FAT_ATTR_DIR32;
        de[1].first_cluster_lo = (uint16_t)(parent_cl & 0xFFFFU);
        de[1].first_cluster_hi = (uint16_t)((parent_cl >> 16) & 0xFFFFU);

        fat32_write_cluster(vol, first_cl, cbuf);
        kfree(cbuf);
    }

    if (fat32_find_free_slot(vol, dp->first_cluster, &slot) < 0) {
        if (first_cl) fat32_write_fat_entry(vol, first_cl, 0U);
        return 0;
    }

    if (fat32_read_sector(vol, slot.lba, sec) < 0) {
        if (first_cl) fat32_write_fat_entry(vol, first_cl, 0U);
        return 0;
    }

    ent = (fat32_dirent_t*)(void*)sec;
    for (i = 0; i < 8U; i++) ent[slot.idx].name[i] = n8[i];
    for (i = 0; i < 3U; i++) ent[slot.idx].ext[i]  = nx[i];
    ent[slot.idx].attr              = (flags & VFS_FLAG_DIR)
                                      ? FAT_ATTR_DIR32 : FAT_ATTR_ARCHIVE32;
    ent[slot.idx].nt_reserved       = 0;
    ent[slot.idx].create_time_tenth = 0;
    ent[slot.idx].create_time       = 0;
    ent[slot.idx].create_date       = 0;
    ent[slot.idx].access_date       = 0;
    ent[slot.idx].mod_time          = 0;
    ent[slot.idx].mod_date          = 0;
    ent[slot.idx].first_cluster_lo  = (uint16_t)(first_cl & 0xFFFFU);
    ent[slot.idx].first_cluster_hi  = (uint16_t)((first_cl >> 16) & 0xFFFFU);
    ent[slot.idx].size              = 0;

    if (fat32_write_sector(vol, slot.lba, sec) < 0) {
        if (first_cl) fat32_write_fat_entry(vol, first_cl, 0U);
        return 0;
    }

    fn = (fat32_node_t*)kmalloc(sizeof(fat32_node_t));
    if (!fn) return 0;

    fn->priv.vol           = vol;
    fn->priv.first_cluster = first_cl;
    fn->priv.size          = 0;
    fn->priv.dirent_lba    = slot.lba;
    fn->priv.dirent_idx    = slot.idx;

    for (i = 0; name[i] && i < VFS_NAME_MAX - 1U; i++) fn->node.name[i] = name[i];
    fn->node.name[i] = '\0';
    fn->node.impl = &fn->priv;
    fn->node.mountpoint = 0;
    fn->node.size = 0;

    if (flags & VFS_FLAG_DIR) {
        fn->node.flags = VFS_FLAG_DIR | VFS_FLAG_DYNAMIC;
        fn->node.ops = &fat32_dir_ops;
    } else {
        fn->node.flags = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
        fn->node.ops = &fat32_file_ops;
    }

    return &fn->node;
}

static int fat32_dir_unlink(vfs_node_t* dir_node, const char* name) {
    fat32_priv_t*           dp  = (fat32_priv_t*)dir_node->impl;
    fat32_vol_t*            vol = dp->vol;
    fat32_finddir_loc_ctx_t ctx;
    uint8_t                 sec[512];
    fat32_dirent_t*         ent;
    uint32_t                fc;

    ctx.target = name;
    ctx.found = 0;
    fat32_walk_dir_loc(vol, dp->first_cluster, fat32_finddir_loc_cb, &ctx);
    if (!ctx.found) return -1;

    if (fat32_read_sector(vol, ctx.dirent_lba, sec) < 0) return -1;
    ent = (fat32_dirent_t*)(void*)sec;
    ent[ctx.dirent_idx].name[0] = (uint8_t)0xE5U;
    if (fat32_write_sector(vol, ctx.dirent_lba, sec) < 0) return -1;

    fc = ((uint32_t)ctx.result.first_cluster_hi << 16) | ctx.result.first_cluster_lo;
    if (fc >= 2U) fat32_free_cluster_chain(vol, fc);
    return 0;
}

/* ============================================================
 *  fat32_mount
 * ============================================================ */

vfs_node_t* fat32_mount(int blkdev_idx) {
    uint8_t      sec[512];
    fat32_bpb_t* bpb;
    fat32_vol_t* vol = 0;
    fat32_priv_t* rp;
    vfs_node_t*   rn;
    uint32_t fat_lba, data_lba;
    int i;

    /* Find a free volume slot */
    for (i = 0; i < FAT32_MAX_VOLUMES; i++) {
        if (!vol_pool[i].used) { vol = &vol_pool[i]; break; }
    }
    if (!vol) return 0;

    /* Read BPB */
    if (blkdev_read(blkdev_idx, 0U, 1U, sec) != 1) return 0;
    if (sec[510] != 0x55 || sec[511] != 0xAA) return 0;

    bpb = (fat32_bpb_t*)(void*)sec;

    /* FAT32 detection */
    if (bpb->bytes_per_sector   != 512) return 0;
    if (bpb->sectors_per_cluster == 0)  return 0;
    if (bpb->fat_count           == 0)  return 0;
    if (bpb->reserved_sectors    == 0)  return 0;
    if (bpb->fat_size_16         != 0)  return 0; /* FAT16/12 have non-zero */
    if (bpb->root_entry_count    != 0)  return 0; /* FAT16/12 have non-zero */
    if (bpb->fat_size_32         == 0)  return 0;
    if (bpb->root_cluster        <  2U) return 0;

    fat_lba  = (uint32_t)bpb->reserved_sectors;
    data_lba = fat_lba
               + (uint32_t)bpb->fat_count * bpb->fat_size_32;

    vol->blkdev_idx          = blkdev_idx;
    vol->bytes_per_sector    = bpb->bytes_per_sector;
    vol->sectors_per_cluster = bpb->sectors_per_cluster;
    vol->fat_count           = bpb->fat_count;
    vol->cluster_size        = (uint32_t)bpb->bytes_per_sector
                               * (uint32_t)bpb->sectors_per_cluster;
    vol->fat_lba             = fat_lba;
    vol->data_lba            = data_lba;
    vol->root_cluster        = bpb->root_cluster;
    vol->used                = 1;

    /* Build static root node */
    rp = &root_priv_pool[i];
    rp->vol           = vol;
    rp->first_cluster = vol->root_cluster;
    rp->size          = 0;
    rp->dirent_lba    = 0;
    rp->dirent_idx    = 0;

    rn = &root_node_pool[i];
    rn->name[0]    = '/';
    rn->name[1]    = '\0';
    rn->flags      = VFS_FLAG_DIR;   /* no VFS_FLAG_DYNAMIC */
    rn->size       = 0;
    rn->ref_count  = 0;
    rn->impl       = rp;
    rn->ops        = &fat32_dir_ops;
    rn->mountpoint = 0;

    return rn;
}
