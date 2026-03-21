#ifndef TTY_VFS_H
#define TTY_VFS_H

#include "vfs.h"

/* Initialise the global TTY VFS node (backed by the terminal).
   Must be called after vfs_init() and terminal_init(). */
void tty_vfs_init(void);

/* Return a pointer to the static TTY node (valid after tty_vfs_init). */
vfs_node_t* tty_vfs_node(void);

/* Return a pointer to the static console node (same backend as tty). */
vfs_node_t* tty_vfs_console_node(void);

#endif
