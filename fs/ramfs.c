#include "ramfs.h"
#include "fs.h"
#include "heap.h"
#include "string.h"

/* ============================================================
 *  File node operations
 * ============================================================ */

static int ramfs_file_read(vfs_node_t* node, unsigned int offset,
                           unsigned int size, unsigned char* buf) {
    unsigned int total   = fs_size(node->name);
    unsigned int to_read;

    if (total == 0 || offset >= total) return 0;

    to_read = total - offset;
    if (to_read > size) to_read = size;

    /* Fast path: no offset, read directly */
    if (offset == 0)
        return fs_read_bytes(node->name, buf, to_read);

    /* Slow path: allocate a temporary buffer and skip 'offset' bytes */
    {
        unsigned char* tmp = (unsigned char*)kmalloc(total);
        int            got;
        unsigned int   available;
        unsigned int   i;

        if (!tmp) return -1;

        got = fs_read_bytes(node->name, tmp, total);
        if (got < 0 || (unsigned int)got <= offset) {
            kfree(tmp);
            return (got < 0) ? -1 : 0;
        }

        available = (unsigned int)got - offset;
        if (available < to_read) to_read = available;

        for (i = 0; i < to_read; i++)
            buf[i] = tmp[offset + i];

        kfree(tmp);
        return (int)to_read;
    }
}

static int ramfs_file_write(vfs_node_t* node, unsigned int offset,
                            unsigned int size, const unsigned char* buf) {
    int result;
    (void)offset; /* ramfs does not support partial / offset writes */

    result = fs_write(node->name, buf, size, 0);
    if (result >= 0)
        node->size = fs_size(node->name);

    return result;
}

static vfs_ops_t ramfs_file_ops = {
    .read    = ramfs_file_read,
    .write   = ramfs_file_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
};

/* ============================================================
 *  Directory node operations
 * ============================================================ */

static int ramfs_dir_readdir(vfs_node_t* node, unsigned int index,
                             char* name_out, unsigned int name_max) {
    const char*  name;
    unsigned int i;

    (void)node;

    name = fs_name_at((int)index);
    if (!name) return -1;

    for (i = 0; name[i] != '\0' && i < name_max - 1U; i++)
        name_out[i] = name[i];
    name_out[i] = '\0';

    return 0;
}

static vfs_node_t* ramfs_dir_finddir(vfs_node_t* node, const char* name) {
    vfs_node_t*  file;
    unsigned int i;

    (void)node;

    if (!fs_exists(name)) return 0;

    file = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!file) return 0;

    /* Copy name into the node's name buffer */
    for (i = 0; name[i] != '\0' && i < VFS_NAME_MAX - 1U; i++)
        file->name[i] = name[i];
    file->name[i] = '\0';

    file->flags      = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
    file->size       = fs_size(name);
    file->impl       = 0;
    file->ops        = &ramfs_file_ops;
    file->mountpoint = 0;

    return file;
}

static vfs_ops_t ramfs_dir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = ramfs_dir_readdir,
    .finddir = ramfs_dir_finddir,
    .open    = 0,
    .close   = 0,
};

/* ============================================================
 *  Root node
 * ============================================================ */

static vfs_node_t ramfs_root_node;

vfs_node_t* ramfs_create_root(void) {
    ramfs_root_node.name[0]    = '/';
    ramfs_root_node.name[1]    = '\0';
    ramfs_root_node.flags      = VFS_FLAG_DIR;  /* not DYNAMIC: static storage */
    ramfs_root_node.size       = 0;
    ramfs_root_node.impl       = 0;
    ramfs_root_node.ops        = &ramfs_dir_ops;
    ramfs_root_node.mountpoint = 0;

    return &ramfs_root_node;
}
