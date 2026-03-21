#include "ramfs.h"
#include "fs.h"
#include "heap.h"
#include "string.h"

/* ============================================================
 *  Virtual directory table (depth-1 directories in the root)
 * ============================================================ */

#define RAMFS_MAX_DIRS 16

typedef struct {
    int  used;
    char name[VFS_NAME_MAX]; /* dir basename, also used as fs.c key prefix */
} ramfs_vdir_t;

static ramfs_vdir_t ramfs_vdirs[RAMFS_MAX_DIRS];

/* Forward declarations */
static vfs_ops_t ramfs_file_ops;
static vfs_ops_t ramfs_vdir_ops;

/* ============================================================
 *  Helpers
 * ============================================================ */

/* Returns 1 if 'name' has a '/' anywhere (belongs to a subdir) */
static int name_has_slash(const char* name) {
    unsigned int i;
    for (i = 0; name[i]; i++)
        if (name[i] == '/') return 1;
    return 0;
}

/* Returns 1 if 'name' starts with "prefix/" */
static int name_has_prefix(const char* name, const char* prefix) {
    unsigned int i;
    for (i = 0; prefix[i] && name[i]; i++)
        if (name[i] != prefix[i]) return 0;
    if (prefix[i] != '\0') return 0;  /* prefix longer than name */
    return name[i] == '/';             /* must be followed by '/' */
}

/* Build "prefix/tail" into out[size] */
static void build_key(const char* prefix, const char* tail,
                      char* out, unsigned int size) {
    unsigned int i = 0, j;
    for (j = 0; prefix[j] && i + 2 < size; j++) out[i++] = prefix[j];
    out[i++] = '/';
    for (j = 0; tail[j]   && i + 1 < size; j++) out[i++] = tail[j];
    out[i] = '\0';
}

/* Allocate and fill a plain-file vfs_node_t backed by fs.c key 'key' */
static vfs_node_t* make_file_node(const char* key) {
    vfs_node_t*  node;
    unsigned int i;

    node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    for (i = 0; key[i] && i < VFS_NAME_MAX - 1U; i++) node->name[i] = key[i];
    node->name[i]    = '\0';
    node->flags      = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
    node->size       = fs_size(key);
    node->impl       = 0;
    node->ops        = &ramfs_file_ops;
    node->mountpoint = 0;
    return node;
}

/* Allocate and fill a virtual-dir vfs_node_t for ramfs_vdirs[slot] */
static vfs_node_t* make_vdir_node(int slot) {
    vfs_node_t*  node;
    unsigned int i;
    const char*  dname = ramfs_vdirs[slot].name;

    node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    for (i = 0; dname[i] && i < VFS_NAME_MAX - 1U; i++) node->name[i] = dname[i];
    node->name[i]    = '\0';
    node->flags      = VFS_FLAG_DIR | VFS_FLAG_DYNAMIC;
    node->size       = 0;
    node->impl       = (void*)ramfs_vdirs[slot].name; /* prefix pointer */
    node->ops        = &ramfs_vdir_ops;
    node->mountpoint = 0;
    return node;
}
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
    /* offset > 0 means caller seeked (e.g. for >>) — map to fs_write append */
    int do_append = (offset > 0) ? 1 : 0;

    result = fs_write(node->name, buf, size, do_append);
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
 *  Virtual-directory node operations  (ops for nodes inside a vdir)
 * ============================================================ */

static int ramfs_vdir_readdir(vfs_node_t* node, unsigned int index,
                              char* name_out, unsigned int name_max) {
    const char*  prefix     = (const char*)node->impl;
    unsigned int prefix_len = str_length(prefix);
    unsigned int found      = 0;
    int          i;

    /* Files in flat store under this prefix */
    for (i = 0; ; i++) {
        const char*  e = fs_name_at(i);
        const char*  stem;
        unsigned int j;

        if (!e) break;
        if (!name_has_prefix(e, prefix)) continue;
        stem = e + prefix_len + 1U; /* skip "prefix/" */
        if (name_has_slash(stem)) continue; /* deeper entries */

        if (found == index) {
            for (j = 0; stem[j] && j < name_max - 1U; j++) name_out[j] = stem[j];
            name_out[j] = '\0';
            return 0;
        }
        found++;
    }

    /* Nested virtual subdirectories: vdirs whose name is "prefix/child" */
    for (i = 0; i < RAMFS_MAX_DIRS; i++) {
        const char*  dname;
        const char*  stem;
        unsigned int j;

        if (!ramfs_vdirs[i].used) continue;
        dname = ramfs_vdirs[i].name;
        if (!name_has_prefix(dname, prefix)) continue;
        stem = dname + prefix_len + 1U;
        if (name_has_slash(stem)) continue; /* only direct children */

        if (found == index) {
            for (j = 0; stem[j] && j < name_max - 1U; j++) name_out[j] = stem[j];
            name_out[j] = '\0';
            return 0;
        }
        found++;
    }

    return -1;
}

static vfs_node_t* ramfs_vdir_finddir(vfs_node_t* node, const char* name) {
    const char*  prefix = (const char*)node->impl;
    char         key[VFS_PATH_MAX];
    int          i;

    build_key(prefix, name, key, sizeof(key));

    /* Check for a nested virtual subdirectory first */
    for (i = 0; i < RAMFS_MAX_DIRS; i++) {
        if (ramfs_vdirs[i].used && str_equals(ramfs_vdirs[i].name, key))
            return make_vdir_node(i);
    }

    /* Fall through to flat file store */
    if (!fs_exists(key)) return 0;
    return make_file_node(key);
}

static vfs_node_t* ramfs_vdir_create(vfs_node_t* node, const char* name,
                                     unsigned int flags) {
    const char*  prefix = (const char*)node->impl;
    char         key[VFS_PATH_MAX];
    unsigned int i;

    build_key(prefix, name, key, sizeof(key));

    if (flags & VFS_FLAG_DIR) {
        /* Create a nested virtual subdirectory (e.g. "home/root"). */
        int slot = -1;
        for (i = 0; i < RAMFS_MAX_DIRS; i++) {
            if (ramfs_vdirs[i].used && str_equals(ramfs_vdirs[i].name, key))
                return make_vdir_node((int)i); /* already exists */
        }
        for (i = 0; i < RAMFS_MAX_DIRS; i++) {
            if (!ramfs_vdirs[i].used) { slot = (int)i; break; }
        }
        if (slot < 0) return 0; /* table full */

        ramfs_vdirs[slot].used = 1;
        for (i = 0; key[i] && i < VFS_NAME_MAX - 1U; i++)
            ramfs_vdirs[slot].name[i] = key[i];
        ramfs_vdirs[slot].name[i] = '\0';
        return make_vdir_node(slot);
    }

    if (fs_write(key, 0, 0, 0) < 0) return 0;
    return make_file_node(key);
}

static int ramfs_vdir_unlink(vfs_node_t* node, const char* name) {
    const char* prefix = (const char*)node->impl;
    char        key[VFS_PATH_MAX];

    build_key(prefix, name, key, sizeof(key));
    return fs_delete(key);
}

static vfs_ops_t ramfs_vdir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = ramfs_vdir_readdir,
    .finddir = ramfs_vdir_finddir,
    .create  = ramfs_vdir_create,
    .unlink  = ramfs_vdir_unlink,
    .open    = 0,
    .close   = 0,
};

/* ============================================================
 *  Root directory node operations
 * ============================================================ */

static int ramfs_dir_readdir(vfs_node_t* node, unsigned int index,
                             char* name_out, unsigned int name_max) {
    unsigned int flat_idx = 0;
    int          i;

    (void)node;

    /* First: flat files (entries with no '/' in name) */
    for (i = 0; ; i++) {
        const char*  e = fs_name_at(i);
        unsigned int j;
        if (!e) break;
        if (name_has_slash(e)) continue; /* belongs to a subdir */
        if (flat_idx == index) {
            for (j = 0; e[j] && j < name_max - 1U; j++) name_out[j] = e[j];
            name_out[j] = '\0';
            return 0;
        }
        flat_idx++;
    }

    /* Second: virtual directories */
    {
        unsigned int want    = index - flat_idx;
        unsigned int dir_idx = 0;
        for (i = 0; i < RAMFS_MAX_DIRS; i++) {
            unsigned int j;
            if (!ramfs_vdirs[i].used) continue;
            if (dir_idx == want) {
                const char* dname = ramfs_vdirs[i].name;
                for (j = 0; dname[j] && j < name_max - 1U; j++) name_out[j] = dname[j];
                name_out[j] = '\0';
                return 0;
            }
            dir_idx++;
        }
    }
    return -1;
}

static vfs_node_t* ramfs_dir_finddir(vfs_node_t* node, const char* name) {
    int i;
    (void)node;

    /* Plain file in flat store */
    if (fs_exists(name))
        return make_file_node(name);

    /* Virtual directory */
    for (i = 0; i < RAMFS_MAX_DIRS; i++) {
        if (ramfs_vdirs[i].used && str_equals(ramfs_vdirs[i].name, name))
            return make_vdir_node(i);
    }

    return 0;
}

static vfs_node_t* ramfs_dir_create(vfs_node_t* node, const char* name,
                                    unsigned int flags) {
    unsigned int i;
    (void)node;

    if (flags & VFS_FLAG_DIR) {
        /* Register a new virtual directory, but never duplicate. */
        int slot = -1;
        for (i = 0; i < RAMFS_MAX_DIRS; i++) {
            if (ramfs_vdirs[i].used && str_equals(ramfs_vdirs[i].name, name))
                return make_vdir_node((int)i); /* already exists */
        }
        for (i = 0; i < RAMFS_MAX_DIRS; i++) {
            if (!ramfs_vdirs[i].used) { slot = (int)i; break; }
        }
        if (slot < 0) return 0; /* table full */

        ramfs_vdirs[slot].used = 1;
        for (i = 0; name[i] && i < VFS_NAME_MAX - 1U; i++)
            ramfs_vdirs[slot].name[i] = name[i];
        ramfs_vdirs[slot].name[i] = '\0';

        return make_vdir_node(slot);
    }

    /* Plain file */
    if (fs_write(name, 0, 0, 0) < 0) return 0;
    return make_file_node(name);
}

static int ramfs_dir_unlink(vfs_node_t* node, const char* name) {
    int i;
    (void)node;

    /* Try flat file first */
    if (fs_exists(name))
        return fs_delete(name);

    /* Try virtual directory (mark as unused) */
    for (i = 0; i < RAMFS_MAX_DIRS; i++) {
        if (ramfs_vdirs[i].used && str_equals(ramfs_vdirs[i].name, name)) {
            ramfs_vdirs[i].used = 0;
            return 0;
        }
    }

    return -1;
}

static vfs_ops_t ramfs_dir_ops = {
    .read    = 0,
    .write   = 0,
    .readdir = ramfs_dir_readdir,
    .finddir = ramfs_dir_finddir,
    .create  = ramfs_dir_create,
    .unlink  = ramfs_dir_unlink,
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
