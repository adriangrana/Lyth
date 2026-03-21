#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

/* Create /dev root node containing device entries such as:
 *   tty, console, null, zero
 */
vfs_node_t* devfs_create_root(void);

/* Convenience accessors for common device nodes. */
vfs_node_t* devfs_tty_node(void);
vfs_node_t* devfs_console_node(void);

#endif
