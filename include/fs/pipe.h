#ifndef PIPE_H
#define PIPE_H

#include "vfs.h"

#define PIPE_FLAG_NONBLOCK 0x0001U

/* Create a connected in-kernel pipe pair.
 * On success returns 0 and sets read_node_out / write_node_out.
 * Nodes are dynamic VFS nodes ready to be installed in fd tables. */
int pipe_create(vfs_node_t** read_node_out, vfs_node_t** write_node_out, unsigned int flags);

/* Helpers used by poll/select to provide proper pipe readiness semantics. */
int pipe_node_is_pipe(const vfs_node_t* node);
int pipe_read_ready(const vfs_node_t* node);
int pipe_write_ready(const vfs_node_t* node);
int pipe_write_has_readers(const vfs_node_t* node);

#endif
