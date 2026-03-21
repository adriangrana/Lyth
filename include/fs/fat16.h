#ifndef FAT16_H
#define FAT16_H

#include <stdint.h>
#include "vfs.h"

/* ============================================================
 *  FAT16 VFS backend  (read + write)
 *
 *  Usage:
 *    vfs_node_t* root = fat16_mount(blkdev_idx);
 *    if (root) vfs_mount("/mnt/hd0p1", root);
 *
 *  Read:  open, read, seek, readdir, finddir.
 *  Write: write (overwrite / extend / append), create (file + dir), unlink.
 *  LFN entries are silently skipped; 8.3 names only (lowercased on read,
 *  uppercased on write).
 *
 *  Maximum simultaneously mounted FAT16 volumes: FAT16_MAX_VOLUMES.
 * ============================================================ */

#define FAT16_MAX_VOLUMES 4

/* Attempt to mount the FAT16 filesystem that lives on block device
 * 'blkdev_idx' (sector 0 is read as the BPB).
 *
 * Returns the root vfs_node_t* on success, NULL on error (invalid BPB,
 * unsupported geometry, or too many volumes already mounted).
 *
 * The returned node pointer is valid for the lifetime of the kernel;
 * pass it directly to vfs_mount(). */
vfs_node_t* fat16_mount(int blkdev_idx);

#endif /* FAT16_H */
