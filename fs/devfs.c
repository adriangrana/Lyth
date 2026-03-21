#include "devfs.h"
#include "tty_vfs.h"
#include "blkdev.h"
#include "heap.h"
#include "string.h"

typedef struct {
    int index;
} dev_blk_priv_t;

static vfs_ops_t   dev_blk_ops;
static vfs_node_t  dev_blk_nodes[BLKDEV_MAX];
static dev_blk_priv_t dev_blk_privs[BLKDEV_MAX];

static int dev_null_read(vfs_node_t* node, unsigned int offset,
                         unsigned int size, unsigned char* buf) {
    (void)node;
    (void)offset;
    (void)size;
    (void)buf;
    return 0;
}

static int dev_null_write(vfs_node_t* node, unsigned int offset,
                          unsigned int size, const unsigned char* buf) {
    (void)node;
    (void)offset;
    (void)buf;
    return (int)size;
}

static int dev_zero_read(vfs_node_t* node, unsigned int offset,
                         unsigned int size, unsigned char* buf) {
    unsigned int i;
    (void)node;
    (void)offset;
    if (!buf) return -1;
    for (i = 0; i < size; i++) buf[i] = 0;
    return (int)size;
}

static int dev_zero_write(vfs_node_t* node, unsigned int offset,
                          unsigned int size, const unsigned char* buf) {
    (void)node;
    (void)offset;
    (void)buf;
    return (int)size;
}

static vfs_ops_t dev_null_ops = {
    .read    = dev_null_read,
    .write   = dev_null_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static vfs_ops_t dev_zero_ops = {
    .read    = dev_zero_read,
    .write   = dev_zero_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static vfs_node_t dev_null_node = {
    .name       = "null",
    .flags      = VFS_FLAG_FILE,
    .size       = 0,
    .ref_count  = 1,
    .impl       = 0,
    .ops        = &dev_null_ops,
    .mountpoint = 0,
};

static vfs_node_t dev_zero_node = {
    .name       = "zero",
    .flags      = VFS_FLAG_FILE,
    .size       = 0,
    .ref_count  = 1,
    .impl       = 0,
    .ops        = &dev_zero_ops,
    .mountpoint = 0,
};

static void devfs_copy_name(char* dst, unsigned int dst_max, const char* src) {
    unsigned int i;
    if (!dst || !src || dst_max == 0) return;
    for (i = 0; src[i] && i < dst_max - 1U; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static vfs_node_t* devfs_blk_node(int index) {
    blkdev_t dev;
    vfs_node_t* node;

    if (index < 0 || index >= BLKDEV_MAX) return 0;
    if (blkdev_get(index, &dev) < 0) return 0;

    node = &dev_blk_nodes[index];
    dev_blk_privs[index].index = index;

    devfs_copy_name(node->name, VFS_NAME_MAX, dev.name);
    node->flags      = VFS_FLAG_FILE;
    node->size       = dev.block_size * dev.block_count;
    node->ref_count  = 1;
    node->impl       = &dev_blk_privs[index];
    node->ops        = &dev_blk_ops;
    node->mountpoint = 0;

    return node;
}

static int dev_blk_read(vfs_node_t* node, unsigned int offset,
                        unsigned int size, unsigned char* buf) {
    dev_blk_priv_t* priv;
    blkdev_t dev;
    unsigned int total_bytes;
    unsigned int remain;
    unsigned int out = 0;
    unsigned int lba;
    unsigned int off;
    unsigned char* sec;

    if (!node || !buf) return -1;
    if (size == 0) return 0;

    priv = (dev_blk_priv_t*)node->impl;
    if (!priv || blkdev_get(priv->index, &dev) < 0) return -1;
    if (dev.block_size == 0) return -1;

    total_bytes = dev.block_size * dev.block_count;
    if (offset >= total_bytes) return 0;
    if (size > total_bytes - offset) size = total_bytes - offset;

    sec = (unsigned char*)kmalloc(dev.block_size);
    if (!sec) return -1;

    remain = size;
    lba = offset / dev.block_size;
    off = offset % dev.block_size;

    while (remain > 0) {
        unsigned int take;
        unsigned int i;

        if (blkdev_read(priv->index, lba, 1, sec) != 1) {
            kfree(sec);
            return out > 0 ? (int)out : -1;
        }

        take = dev.block_size - off;
        if (take > remain) take = remain;

        for (i = 0; i < take; i++)
            buf[out + i] = sec[off + i];

        out += take;
        remain -= take;
        lba++;
        off = 0;
    }

    kfree(sec);
    return (int)out;
}

static int dev_blk_write(vfs_node_t* node, unsigned int offset,
                         unsigned int size, const unsigned char* buf) {
    dev_blk_priv_t* priv;
    blkdev_t dev;
    unsigned int total_bytes;
    unsigned int remain;
    unsigned int written = 0;
    unsigned int lba;
    unsigned int off;
    unsigned char* sec;

    if (!node || !buf) return -1;
    if (size == 0) return 0;

    priv = (dev_blk_priv_t*)node->impl;
    if (!priv || blkdev_get(priv->index, &dev) < 0) return -1;
    if (dev.block_size == 0) return -1;

    total_bytes = dev.block_size * dev.block_count;
    if (offset >= total_bytes) return 0;
    if (size > total_bytes - offset) size = total_bytes - offset;

    sec = (unsigned char*)kmalloc(dev.block_size);
    if (!sec) return -1;

    remain = size;
    lba = offset / dev.block_size;
    off = offset % dev.block_size;

    while (remain > 0) {
        unsigned int take;
        unsigned int i;

        take = dev.block_size - off;
        if (take > remain) take = remain;

        if (off != 0 || take != dev.block_size) {
            if (blkdev_read(priv->index, lba, 1, sec) != 1) {
                kfree(sec);
                return written > 0 ? (int)written : -1;
            }
        }

        for (i = 0; i < take; i++)
            sec[off + i] = buf[written + i];

        if (blkdev_write(priv->index, lba, 1, sec) != 1) {
            kfree(sec);
            return written > 0 ? (int)written : -1;
        }

        written += take;
        remain -= take;
        lba++;
        off = 0;
    }

    kfree(sec);
    return (int)written;
}

static vfs_ops_t dev_blk_ops = {
    .read    = dev_blk_read,
    .write   = dev_blk_write,
    .readdir = 0,
    .finddir = 0,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static int devfs_readdir(vfs_node_t* node, unsigned int index,
                         char* name_out, unsigned int name_max) {
    static const char* names[] = { "tty", "console", "null", "zero" };
    unsigned int fixed = (unsigned int)(sizeof(names) / sizeof(names[0]));
    unsigned int i;
    const char* src;

    (void)node;
    if (!name_out || name_max == 0) return -1;

    if (index < fixed) {
        src = names[index];
        for (i = 0; src[i] && i < name_max - 1U; i++) name_out[i] = src[i];
        name_out[i] = '\0';
        return 0;
    }

    {
        unsigned int dyn = index - fixed;
        for (i = 0; i < BLKDEV_MAX; i++) {
            blkdev_t dev;
            if (blkdev_get((int)i, &dev) < 0) continue;
            if (dyn == 0U) {
                unsigned int j;
                for (j = 0; dev.name[j] && j < name_max - 1U; j++)
                    name_out[j] = dev.name[j];
                name_out[j] = '\0';
                return 0;
            }
            dyn--;
        }
    }

    return -1;
}

static vfs_node_t* devfs_finddir(vfs_node_t* node, const char* name) {
    (void)node;
    if (!name) return 0;

    if (str_equals(name, "tty"))     return tty_vfs_node();
    if (str_equals(name, "console")) return tty_vfs_console_node();
    if (str_equals(name, "null"))    return &dev_null_node;
    if (str_equals(name, "zero"))    return &dev_zero_node;

    {
        int idx = blkdev_find(name);
        if (idx >= 0) return devfs_blk_node(idx);
    }
    return 0;
}

static vfs_ops_t devfs_dir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = devfs_readdir,
    .finddir = devfs_finddir,
    .open    = 0,
    .close   = 0,
    .create  = 0,
    .unlink  = 0,
};

static vfs_node_t devfs_root = {
    .name       = "dev",
    .flags      = VFS_FLAG_DIR,
    .size       = 0,
    .ref_count  = 1,
    .impl       = 0,
    .ops        = &devfs_dir_ops,
    .mountpoint = 0,
};

vfs_node_t* devfs_create_root(void) {
    return &devfs_root;
}

vfs_node_t* devfs_tty_node(void) {
    return tty_vfs_node();
}

vfs_node_t* devfs_console_node(void) {
    return tty_vfs_console_node();
}
