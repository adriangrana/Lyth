/* ============================================================
 *  tty_vfs.c  —  Virtual TTY node for stdin/stdout/stderr
 *
 *  write  → terminal_put_char (every byte forwarded to the console)
 *  read   → returns 0 bytes (non-blocking; keyboard demux per-process
 *            is not yet implemented)
 *  open/close/seek are no-ops.
 * ============================================================ */

#include "tty_vfs.h"
#include "vfs.h"
#include "terminal.h"

static int tty_write(vfs_node_t* node, unsigned int offset,
                     unsigned int size, const unsigned char* buf)
{
    unsigned int i;
    (void)node;
    (void)offset;
    if (!buf || size == 0) return 0;
    for (i = 0; i < size; i++)
        terminal_put_char((char)buf[i]);
    return (int)size;
}

static int tty_read(vfs_node_t* node, unsigned int offset,
                    unsigned int size, unsigned char* buf)
{
    (void)node; (void)offset; (void)size; (void)buf;
    /* Non-blocking: no bytes available. */
    return 0;
}

static vfs_ops_t tty_ops = {
    .read    = tty_read,
    .write   = tty_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static vfs_node_t tty_node = {
    .name      = "tty",
    .flags     = VFS_FLAG_FILE,
    .size      = 0,
    .ref_count = 1,   /* permanent system reference — never freed */
    .impl      = 0,
    .ops       = &tty_ops,
    .mountpoint = 0,
};

static vfs_node_t console_node = {
    .name      = "console",
    .flags     = VFS_FLAG_FILE,
    .size      = 0,
    .ref_count = 1,   /* permanent system reference — never freed */
    .impl      = 0,
    .ops       = &tty_ops,
    .mountpoint = 0,
};

void tty_vfs_init(void) {
    /* Nothing to do — the node is statically initialised. */
}

vfs_node_t* tty_vfs_node(void) {
    return &tty_node;
}

vfs_node_t* tty_vfs_console_node(void) {
    return &console_node;
}
