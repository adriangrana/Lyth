#include "fat16.h"
#include "fat32.h"
#include "fat_fsck.h"
#include "blkdev.h"
#include "heap.h"
#include <stdint.h>

#define FAT_ATTR_VOLID 0x08U
#define FAT_ATTR_DIR   0x10U
#define FAT_ATTR_LFN   0x0FU

#define FAT16_EOC_MIN     0xFFF8U
#define FAT16_BAD_CLUSTER 0xFFF7U

#define FAT32_EOC_MIN      0x0FFFFFF8UL
#define FAT32_BAD_CLUSTER  0x0FFFFFF7UL
#define FAT32_CLUSTER_MASK 0x0FFFFFFFUL

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
    uint16_t sectors_per_fat;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} fat16_bpb_t;

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
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fs_type[8];
} fat32_bpb_t;

typedef struct __attribute__((packed)) {
    uint8_t  name[8];
    uint8_t  ext[3];
    uint8_t  attr;
    uint8_t  nt_reserved;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t access_date;
    uint16_t first_cluster_hi;
    uint16_t mod_time;
    uint16_t mod_date;
    uint16_t first_cluster_lo;
    uint32_t size;
} fat_dirent_t;

typedef struct __attribute__((packed)) {
    uint8_t  order;
    uint16_t name1[5];
    uint8_t  attr;
    uint8_t  type;
    uint8_t  checksum;
    uint16_t name2[6];
    uint16_t first_cl;
    uint16_t name3[2];
} fat_lfn_entry_t;

typedef struct {
    int      blkdev_idx;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  fat_count;
    uint16_t sectors_per_fat;
    uint16_t root_entry_count;
    uint32_t total_sectors;
    uint32_t fat_lba;
    uint32_t root_lba;
    uint32_t data_lba;
    uint32_t total_clusters;
    uint16_t max_cluster;
} fat16_fsck_ctx_t;

typedef struct {
    int      blkdev_idx;
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint8_t  fat_count;
    uint32_t fat_size_32;
    uint32_t total_sectors;
    uint32_t fat_lba;
    uint32_t data_lba;
    uint32_t root_cluster;
    uint32_t total_clusters;
    uint32_t max_cluster;
} fat32_fsck_ctx_t;

static void fsck_zero_report(fat_fsck_report_t* out) {
    uint8_t* p;
    uint32_t i;
    if (!out) return;
    p = (uint8_t*)(void*)out;
    for (i = 0; i < (uint32_t)sizeof(*out); i++) p[i] = 0;
}

static int fsck_read_sector(int dev_idx, uint32_t lba, uint8_t* buf) {
    return (blkdev_read(dev_idx, lba, 1U, buf) == 1) ? 0 : -1;
}

static int fsck_is_pow2_u8(uint8_t v) {
    return (v != 0U) && ((v & (uint8_t)(v - 1U)) == 0U);
}

/* ---------------- FAT16 ---------------- */

static int fat16_parse_ctx(int blkdev_idx, fat16_fsck_ctx_t* ctx) {
    uint8_t      sec[512];
    fat16_bpb_t* bpb;
    blkdev_t     dev;
    uint32_t     root_dir_sectors;
    uint32_t     data_sectors;

    if (!ctx) return -1;
    if (blkdev_get(blkdev_idx, &dev) < 0) return -1;
    if (dev.block_size != 512U || dev.block_count == 0U) return -1;

    if (fsck_read_sector(blkdev_idx, 0U, sec) < 0) return -1;
    if (sec[510] != 0x55U || sec[511] != 0xAAU) return -1;

    bpb = (fat16_bpb_t*)(void*)sec;

    if (bpb->bytes_per_sector != 512U) return -1;
    if (!fsck_is_pow2_u8(bpb->sectors_per_cluster)) return -1;
    if (bpb->reserved_sectors == 0U) return -1;
    if (bpb->fat_count == 0U) return -1;
    if (bpb->sectors_per_fat == 0U) return -1;
    if (bpb->root_entry_count == 0U) return -1;

    ctx->total_sectors = (bpb->total_sectors_16 != 0U)
        ? (uint32_t)bpb->total_sectors_16
        : bpb->total_sectors_32;
    if (ctx->total_sectors == 0U) return -1;
    if (ctx->total_sectors > dev.block_count) return -1;

    root_dir_sectors = ((uint32_t)bpb->root_entry_count * 32U + 511U) / 512U;
    ctx->fat_lba = (uint32_t)bpb->reserved_sectors;
    ctx->root_lba = ctx->fat_lba + (uint32_t)bpb->fat_count * (uint32_t)bpb->sectors_per_fat;
    ctx->data_lba = ctx->root_lba + root_dir_sectors;

    if (ctx->data_lba >= ctx->total_sectors) return -1;

    data_sectors = ctx->total_sectors - ctx->data_lba;
    ctx->total_clusters = data_sectors / (uint32_t)bpb->sectors_per_cluster;
    if (ctx->total_clusters < 1U || ctx->total_clusters > 0xFFFDUL) return -1;

    ctx->max_cluster = (uint16_t)(ctx->total_clusters + 1U);
    ctx->blkdev_idx = blkdev_idx;
    ctx->bytes_per_sector = bpb->bytes_per_sector;
    ctx->sectors_per_cluster = bpb->sectors_per_cluster;
    ctx->fat_count = bpb->fat_count;
    ctx->sectors_per_fat = bpb->sectors_per_fat;
    ctx->root_entry_count = bpb->root_entry_count;
    return 0;
}

static uint16_t fat16_get_entry(const fat16_fsck_ctx_t* ctx, uint16_t cluster) {
    uint8_t  sec[512];
    uint32_t byte_off;
    uint32_t fat_sec;
    uint32_t sec_off;

    byte_off = (uint32_t)cluster * 2U;
    fat_sec  = ctx->fat_lba + byte_off / 512U;
    sec_off  = byte_off % 512U;
    if (sec_off + 1U >= 512U) return 0xFFFFU;
    if (fsck_read_sector(ctx->blkdev_idx, fat_sec, sec) < 0) return 0xFFFFU;
    return (uint16_t)((uint16_t)sec[sec_off] | ((uint16_t)sec[sec_off + 1U] << 8));
}

static int fat16_validate_chain(const fat16_fsck_ctx_t* ctx,
                                uint16_t start_cluster,
                                fat_fsck_report_t* rep) {
    uint16_t cur;
    uint32_t steps = 0;

    if (start_cluster < 2U || start_cluster > ctx->max_cluster) {
        rep->invalid_start_cluster++;
        rep->errors++;
        return -1;
    }

    cur = start_cluster;
    while (cur >= 2U && cur <= ctx->max_cluster) {
        uint16_t nxt = fat16_get_entry(ctx, cur);

        if (nxt == 0U || nxt == 1U) {
            rep->fat_out_of_range++;
            rep->errors++;
            return -1;
        }
        if (nxt == FAT16_BAD_CLUSTER) {
            rep->fat_bad_clusters++;
            rep->warnings++;
            return 0;
        }
        if (nxt >= FAT16_EOC_MIN) return 0;
        if (nxt > ctx->max_cluster) {
            rep->fat_out_of_range++;
            rep->errors++;
            return -1;
        }

        cur = nxt;
        steps++;
        if (steps > ctx->total_clusters + 1U) {
            rep->fat_loops++;
            rep->errors++;
            return -1;
        }
    }

    rep->fat_out_of_range++;
    rep->errors++;
    return -1;
}

static int fat16_scan_dir(const fat16_fsck_ctx_t* ctx,
                          uint16_t dir_cluster,
                          uint8_t* visited_dirs,
                          fat_fsck_report_t* rep);

static int fat16_scan_dir_entries(const fat16_fsck_ctx_t* ctx,
                                  fat_dirent_t* ent,
                                  uint32_t count,
                                  uint8_t* visited_dirs,
                                  fat_fsck_report_t* rep) {
    uint32_t i;

    for (i = 0; i < count; i++) {
        uint16_t first_cluster;
        int      is_dir;

        if (ent[i].name[0] == 0x00U) return 0;
        if ((uint8_t)ent[i].name[0] == 0xE5U) continue;

        if (ent[i].attr == FAT_ATTR_LFN) {
            fat_lfn_entry_t* l = (fat_lfn_entry_t*)(void*)&ent[i];
            if (l->first_cl != 0U) {
                rep->dir_corrupt_entries++;
                rep->errors++;
            }
            continue;
        }

        if (ent[i].name[0] == ' ' || ent[i].attr == 0xFFU) {
            rep->dir_corrupt_entries++;
            rep->errors++;
        }

        if (ent[i].attr & FAT_ATTR_VOLID) continue;
        if (ent[i].name[0] == '.') continue;

        if (ent[i].first_cluster_hi != 0U) {
            rep->dir_corrupt_entries++;
            rep->errors++;
        }

        first_cluster = ent[i].first_cluster_lo;
        is_dir = ((ent[i].attr & FAT_ATTR_DIR) != 0U) ? 1 : 0;

        if (is_dir) {
            rep->dirs_checked++;
            if (first_cluster < 2U || first_cluster > ctx->max_cluster) {
                rep->invalid_start_cluster++;
                rep->errors++;
                continue;
            }

            fat16_validate_chain(ctx, first_cluster, rep);

            if (!visited_dirs[first_cluster]) {
                visited_dirs[first_cluster] = 1U;
                fat16_scan_dir(ctx, first_cluster, visited_dirs, rep);
            }
            continue;
        }

        rep->files_checked++;
        if (ent[i].size > 0U) {
            fat16_validate_chain(ctx, first_cluster, rep);
        } else if (first_cluster > ctx->max_cluster) {
            rep->invalid_start_cluster++;
            rep->errors++;
        }
    }

    return 0;
}

static int fat16_scan_dir(const fat16_fsck_ctx_t* ctx,
                          uint16_t dir_cluster,
                          uint8_t* visited_dirs,
                          fat_fsck_report_t* rep) {
    uint8_t sec[512];

    if (dir_cluster == 0U) {
        uint32_t root_dir_sectors = ((uint32_t)ctx->root_entry_count * 32U + 511U) / 512U;
        uint32_t s;
        for (s = 0; s < root_dir_sectors; s++) {
            fat_dirent_t* ent;
            if (fsck_read_sector(ctx->blkdev_idx, ctx->root_lba + s, sec) < 0) {
                rep->errors++;
                return -1;
            }
            ent = (fat_dirent_t*)(void*)sec;
            fat16_scan_dir_entries(ctx, ent, 512U / sizeof(fat_dirent_t), visited_dirs, rep);
        }
        return 0;
    }

    {
        uint16_t cur = dir_cluster;
        uint32_t hops = 0;

        while (cur >= 2U && cur <= ctx->max_cluster) {
            uint32_t base_lba = ctx->data_lba + ((uint32_t)cur - 2U) * (uint32_t)ctx->sectors_per_cluster;
            uint32_t s;

            for (s = 0; s < (uint32_t)ctx->sectors_per_cluster; s++) {
                fat_dirent_t* ent;
                if (fsck_read_sector(ctx->blkdev_idx, base_lba + s, sec) < 0) {
                    rep->errors++;
                    return -1;
                }
                ent = (fat_dirent_t*)(void*)sec;
                if (fat16_scan_dir_entries(ctx, ent, 512U / sizeof(fat_dirent_t), visited_dirs, rep) != 0)
                    return 0;
            }

            cur = fat16_get_entry(ctx, cur);
            if (cur >= FAT16_EOC_MIN) break;
            hops++;
            if (hops > ctx->total_clusters + 1U) {
                rep->fat_loops++;
                rep->errors++;
                break;
            }
        }
    }

    return 0;
}

int fat16_fsck_lite(int blkdev_idx, fat_fsck_report_t* out) {
    fat16_fsck_ctx_t ctx;
    fat_fsck_report_t local;
    fat_fsck_report_t* rep;
    uint8_t* visited_dirs;
    uint32_t c;

    fsck_zero_report(&local);
    rep = out ? out : &local;
    fsck_zero_report(rep);

    if (fat16_parse_ctx(blkdev_idx, &ctx) < 0) return -1;

    for (c = 2U; c <= (uint32_t)ctx.max_cluster; c++) {
        uint16_t e = fat16_get_entry(&ctx, (uint16_t)c);
        if (e == 0U) continue;
        if (e == FAT16_BAD_CLUSTER) {
            rep->fat_bad_clusters++;
            rep->warnings++;
            continue;
        }
        if (e >= FAT16_EOC_MIN) continue;
        if (e < 2U || e > ctx.max_cluster) {
            rep->fat_out_of_range++;
            rep->errors++;
        }
    }

    visited_dirs = (uint8_t*)kmalloc((uint32_t)ctx.max_cluster + 2U);
    if (!visited_dirs) {
        rep->errors++;
        return 0;
    }
    for (c = 0; c < (uint32_t)ctx.max_cluster + 2U; c++) visited_dirs[c] = 0U;

    fat16_scan_dir(&ctx, 0U, visited_dirs, rep);

    kfree(visited_dirs);
    return 0;
}

/* ---------------- FAT32 ---------------- */

static int fat32_parse_ctx(int blkdev_idx, fat32_fsck_ctx_t* ctx) {
    uint8_t      sec[512];
    fat32_bpb_t* bpb;
    blkdev_t     dev;
    uint32_t     data_sectors;

    if (!ctx) return -1;
    if (blkdev_get(blkdev_idx, &dev) < 0) return -1;
    if (dev.block_size != 512U || dev.block_count == 0U) return -1;

    if (fsck_read_sector(blkdev_idx, 0U, sec) < 0) return -1;
    if (sec[510] != 0x55U || sec[511] != 0xAAU) return -1;

    bpb = (fat32_bpb_t*)(void*)sec;

    if (bpb->bytes_per_sector != 512U) return -1;
    if (!fsck_is_pow2_u8(bpb->sectors_per_cluster)) return -1;
    if (bpb->reserved_sectors == 0U) return -1;
    if (bpb->fat_count == 0U) return -1;
    if (bpb->fat_size_16 != 0U) return -1;
    if (bpb->root_entry_count != 0U) return -1;
    if (bpb->fat_size_32 == 0U) return -1;

    ctx->total_sectors = (bpb->total_sectors_16 != 0U)
        ? (uint32_t)bpb->total_sectors_16
        : bpb->total_sectors_32;
    if (ctx->total_sectors == 0U) return -1;
    if (ctx->total_sectors > dev.block_count) return -1;

    ctx->fat_lba = (uint32_t)bpb->reserved_sectors;
    ctx->data_lba = ctx->fat_lba + (uint32_t)bpb->fat_count * bpb->fat_size_32;
    if (ctx->data_lba >= ctx->total_sectors) return -1;

    data_sectors = ctx->total_sectors - ctx->data_lba;
    ctx->total_clusters = data_sectors / (uint32_t)bpb->sectors_per_cluster;
    if (ctx->total_clusters < 1U || ctx->total_clusters > 0x0FFFFFFDUL) return -1;

    ctx->max_cluster = ctx->total_clusters + 1U;
    ctx->root_cluster = bpb->root_cluster & FAT32_CLUSTER_MASK;
    if (ctx->root_cluster < 2U || ctx->root_cluster > ctx->max_cluster) return -1;

    ctx->blkdev_idx = blkdev_idx;
    ctx->bytes_per_sector = bpb->bytes_per_sector;
    ctx->sectors_per_cluster = bpb->sectors_per_cluster;
    ctx->fat_count = bpb->fat_count;
    ctx->fat_size_32 = bpb->fat_size_32;
    return 0;
}

static uint32_t fat32_get_entry(const fat32_fsck_ctx_t* ctx, uint32_t cluster) {
    uint8_t  sec[512];
    uint32_t byte_off;
    uint32_t fat_sec;
    uint32_t sec_off;

    byte_off = cluster * 4U;
    fat_sec  = ctx->fat_lba + byte_off / 512U;
    sec_off  = byte_off % 512U;
    if (sec_off + 3U >= 512U) return FAT32_EOC_MIN;
    if (fsck_read_sector(ctx->blkdev_idx, fat_sec, sec) < 0) return FAT32_EOC_MIN;

    return (((uint32_t)sec[sec_off])
          | ((uint32_t)sec[sec_off + 1U] << 8)
          | ((uint32_t)sec[sec_off + 2U] << 16)
          | ((uint32_t)sec[sec_off + 3U] << 24)) & FAT32_CLUSTER_MASK;
}

static int fat32_validate_chain(const fat32_fsck_ctx_t* ctx,
                                uint32_t start_cluster,
                                fat_fsck_report_t* rep) {
    uint32_t cur;
    uint32_t steps = 0;

    if (start_cluster < 2U || start_cluster > ctx->max_cluster) {
        rep->invalid_start_cluster++;
        rep->errors++;
        return -1;
    }

    cur = start_cluster;
    while (cur >= 2U && cur <= ctx->max_cluster) {
        uint32_t nxt = fat32_get_entry(ctx, cur);

        if (nxt == 0U || nxt == 1U) {
            rep->fat_out_of_range++;
            rep->errors++;
            return -1;
        }
        if (nxt == FAT32_BAD_CLUSTER) {
            rep->fat_bad_clusters++;
            rep->warnings++;
            return 0;
        }
        if (nxt >= FAT32_EOC_MIN) return 0;
        if (nxt > ctx->max_cluster) {
            rep->fat_out_of_range++;
            rep->errors++;
            return -1;
        }

        cur = nxt;
        steps++;
        if (steps > ctx->total_clusters + 1U) {
            rep->fat_loops++;
            rep->errors++;
            return -1;
        }
    }

    rep->fat_out_of_range++;
    rep->errors++;
    return -1;
}

static int fat32_scan_dir(const fat32_fsck_ctx_t* ctx,
                          uint32_t dir_cluster,
                          uint8_t* visited_dirs,
                          fat_fsck_report_t* rep) {
    uint8_t sec[512];
    uint32_t cur = dir_cluster;
    uint32_t hops = 0;

    while (cur >= 2U && cur <= ctx->max_cluster) {
        uint32_t base_lba = ctx->data_lba + (cur - 2U) * (uint32_t)ctx->sectors_per_cluster;
        uint32_t s;

        for (s = 0; s < (uint32_t)ctx->sectors_per_cluster; s++) {
            fat_dirent_t* ent;
            uint32_t i;

            if (fsck_read_sector(ctx->blkdev_idx, base_lba + s, sec) < 0) {
                rep->errors++;
                return -1;
            }

            ent = (fat_dirent_t*)(void*)sec;
            for (i = 0; i < 512U / sizeof(fat_dirent_t); i++) {
                uint32_t first_cluster;
                int is_dir;

                if (ent[i].name[0] == 0x00U) return 0;
                if ((uint8_t)ent[i].name[0] == 0xE5U) continue;

                if (ent[i].attr == FAT_ATTR_LFN) {
                    fat_lfn_entry_t* l = (fat_lfn_entry_t*)(void*)&ent[i];
                    if (l->first_cl != 0U) {
                        rep->dir_corrupt_entries++;
                        rep->errors++;
                    }
                    continue;
                }

                if (ent[i].name[0] == ' ' || ent[i].attr == 0xFFU) {
                    rep->dir_corrupt_entries++;
                    rep->errors++;
                }

                if (ent[i].attr & FAT_ATTR_VOLID) continue;
                if (ent[i].name[0] == '.') continue;

                first_cluster = ((((uint32_t)ent[i].first_cluster_hi) << 16)
                                | (uint32_t)ent[i].first_cluster_lo) & FAT32_CLUSTER_MASK;
                is_dir = ((ent[i].attr & FAT_ATTR_DIR) != 0U) ? 1 : 0;

                if (is_dir) {
                    rep->dirs_checked++;
                    if (first_cluster < 2U || first_cluster > ctx->max_cluster) {
                        rep->invalid_start_cluster++;
                        rep->errors++;
                        continue;
                    }

                    fat32_validate_chain(ctx, first_cluster, rep);

                    if (!visited_dirs[first_cluster]) {
                        visited_dirs[first_cluster] = 1U;
                        fat32_scan_dir(ctx, first_cluster, visited_dirs, rep);
                    }
                    continue;
                }

                rep->files_checked++;
                if (ent[i].size > 0U) {
                    fat32_validate_chain(ctx, first_cluster, rep);
                } else if (first_cluster > ctx->max_cluster) {
                    rep->invalid_start_cluster++;
                    rep->errors++;
                }
            }
        }

        cur = fat32_get_entry(ctx, cur);
        if (cur >= FAT32_EOC_MIN) break;
        hops++;
        if (hops > ctx->total_clusters + 1U) {
            rep->fat_loops++;
            rep->errors++;
            break;
        }
    }

    return 0;
}

int fat32_fsck_lite(int blkdev_idx, fat_fsck_report_t* out) {
    fat32_fsck_ctx_t ctx;
    fat_fsck_report_t local;
    fat_fsck_report_t* rep;
    uint8_t* visited_dirs;
    uint32_t c;

    fsck_zero_report(&local);
    rep = out ? out : &local;
    fsck_zero_report(rep);

    if (fat32_parse_ctx(blkdev_idx, &ctx) < 0) return -1;

    for (c = 2U; c <= ctx.max_cluster; c++) {
        uint32_t e = fat32_get_entry(&ctx, c);
        if (e == 0U) continue;
        if (e == FAT32_BAD_CLUSTER) {
            rep->fat_bad_clusters++;
            rep->warnings++;
            continue;
        }
        if (e >= FAT32_EOC_MIN) continue;
        if (e < 2U || e > ctx.max_cluster) {
            rep->fat_out_of_range++;
            rep->errors++;
        }
    }

    visited_dirs = (uint8_t*)kmalloc(ctx.max_cluster + 2U);
    if (!visited_dirs) {
        rep->errors++;
        return 0;
    }
    for (c = 0; c < ctx.max_cluster + 2U; c++) visited_dirs[c] = 0U;

    visited_dirs[ctx.root_cluster] = 1U;
    fat32_scan_dir(&ctx, ctx.root_cluster, visited_dirs, rep);

    kfree(visited_dirs);
    return 0;
}
