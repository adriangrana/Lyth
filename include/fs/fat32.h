#ifndef FAT32_H
#define FAT32_H

#include <stdint.h>
#include "vfs.h"
#include "fat_fsck.h"

/* ============================================================
 *  FAT32 VFS backend  (read-only + LFN)
 *
 *  Usage:
 *    vfs_node_t* root = fat32_mount(blkdev_idx);
 *    if (root) vfs_mount("/mnt/hd0p1", root);
 *
 *  Read:  open, read, seek, readdir, finddir.
 *  Write: not supported (read-only).
 *  LFN:   Long File Names are read transparently; the full
 *         Unicode name is returned as lowercase ASCII.
 *
 *  Maximum simultaneously mounted FAT32 volumes: FAT32_MAX_VOLUMES.
 * ============================================================ */

#define FAT32_MAX_VOLUMES 4

/* Attempt to mount the FAT32 filesystem on block device 'blkdev_idx'.
 * Returns the root vfs_node_t* on success, NULL on error (invalid BPB,
 * not FAT32, or too many volumes already mounted).
 *
 * The returned node is valid for the lifetime of the kernel. */
vfs_node_t* fat32_mount(int blkdev_idx);

/*
 * Run a lightweight integrity check for a FAT32 volume.
 *
 * Checks include:
 *  - boot sector layout sanity
 *  - FAT entry range sanity
 *  - chain loop detection
 *  - obvious corrupt directory entries
 *  - invalid first-cluster values in file entries
 *
 * Returns 0 on success (filesystem recognized and scanned), -1 on failure
 * (not FAT32 or I/O error while scanning metadata).
 */
int fat32_fsck_lite(int blkdev_idx, fat_fsck_report_t* out);

#endif /* FAT32_H */
