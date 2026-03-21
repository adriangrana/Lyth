#ifndef RAMFS_H
#define RAMFS_H

#include "vfs.h"

/*
 * RamFS — a thin VFS backend that wraps the existing flat in-memory
 * filesystem (fs.c / fs.h).
 *
 * Typical use:
 *   vfs_init();
 *   vfs_mount("/", ramfs_create_root());
 */

/* Create (and return a pointer to) the static root directory node for the
 * RAM filesystem.  The returned pointer is valid for the lifetime of the
 * kernel; do not free it. */
vfs_node_t* ramfs_create_root(void);

#endif
