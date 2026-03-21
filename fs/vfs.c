#include "vfs.h"
#include "heap.h"
#include "string.h"

/* ============================================================
 *  Mount table
 * ============================================================ */

typedef struct {
    char        path[VFS_PATH_MAX];
    vfs_node_t* root;
    int         used;
} vfs_mount_entry_t;

static vfs_mount_entry_t mounts[VFS_MAX_MOUNTS];

/* ============================================================
 *  File-descriptor table  (kernel-global for now)
 * ============================================================ */

typedef struct {
    vfs_node_t*  node;
    unsigned int offset;
    int          used;
} vfs_fd_entry_t;

static vfs_fd_entry_t fd_table[VFS_MAX_FD];

/* ============================================================
 *  Init
 * ============================================================ */

void vfs_init(void) {
    int i;
    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        mounts[i].used = 0;
        mounts[i].root = 0;
        mounts[i].path[0] = '\0';
    }
    for (i = 0; i < VFS_MAX_FD; i++) {
        fd_table[i].used   = 0;
        fd_table[i].node   = 0;
        fd_table[i].offset = 0;
    }
}

/* ============================================================
 *  Mount
 * ============================================================ */

int vfs_mount(const char* path, vfs_node_t* root) {
    int i;
    unsigned int j;

    if (!path || !root) return -1;

    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        if (!mounts[i].used) {
            for (j = 0; path[j] != '\0' && j < VFS_PATH_MAX - 1U; j++)
                mounts[i].path[j] = path[j];
            mounts[i].path[j] = '\0';
            mounts[i].root = root;
            mounts[i].used = 1;
            return 0;
        }
    }
    return -1; /* mount table full */
}

/* ============================================================
 *  Node-level operations
 * ============================================================ */

/* If node has a mountpoint redirect, use the mounted root instead. */
static vfs_node_t* vfs_effective(vfs_node_t* node) {
    return (node && node->mountpoint) ? node->mountpoint : node;
}

int vfs_node_read(vfs_node_t* node, unsigned int offset,
                  unsigned int size, unsigned char* buf) {
    vfs_node_t* n;
    if (!node || !buf || size == 0) return -1;
    n = vfs_effective(node);
    if (!n->ops || !n->ops->read) return -1;
    return n->ops->read(n, offset, size, buf);
}

int vfs_node_write(vfs_node_t* node, unsigned int offset,
                   unsigned int size, const unsigned char* buf) {
    vfs_node_t* n;
    if (!node || !buf || size == 0) return -1;
    n = vfs_effective(node);
    if (!n->ops || !n->ops->write) return -1;
    return n->ops->write(n, offset, size, buf);
}

int vfs_node_readdir(vfs_node_t* node, unsigned int index,
                     char* name_out, unsigned int name_max) {
    vfs_node_t* n;
    if (!node || !name_out || name_max == 0) return -1;
    n = vfs_effective(node);
    if (!n->ops || !n->ops->readdir) return -1;
    return n->ops->readdir(n, index, name_out, name_max);
}

vfs_node_t* vfs_node_finddir(vfs_node_t* node, const char* name) {
    vfs_node_t* n;
    if (!node || !name) return 0;
    n = vfs_effective(node);
    if (!n->ops || !n->ops->finddir) return 0;
    return n->ops->finddir(n, name);
}

/* ============================================================
 *  Path resolution
 * ============================================================ */

/*
 * Find the deepest matching mount for 'path'.
 * Sets *rest to the portion of path after the mount prefix.
 * Returns the mount's root node, or NULL if no mount matches.
 */
static vfs_node_t* vfs_find_mount(const char* path, const char** rest) {
    vfs_node_t*  best_root = 0;
    unsigned int best_len  = 0;
    int i;

    for (i = 0; i < VFS_MAX_MOUNTS; i++) {
        unsigned int j;
        const char*  mp;

        if (!mounts[i].used) continue;
        mp = mounts[i].path;

        /* Special case: mount at "/" matches any absolute path */
        if (mp[0] == '/' && mp[1] == '\0') {
            if (path[0] == '/' && best_len == 0) {
                best_root = mounts[i].root;
                best_len  = 1;
            }
            continue;
        }

        /* General case: compare prefix component by component */
        for (j = 0; mp[j] != '\0' && path[j] != '\0' && mp[j] == path[j]; j++)
            ;

        if (mp[j] == '\0' && j > best_len) {
            /* Ensure we stop on a path separator or end-of-string */
            if (path[j] == '/' || path[j] == '\0') {
                best_root = mounts[i].root;
                best_len  = j;
            }
        }
    }

    if (best_root && rest) {
        *rest = path + best_len;
        if (**rest == '/') (*rest)++;   /* skip leading separator */
    }

    return best_root;
}

vfs_node_t* vfs_resolve(const char* path) {
    const char*  rest = 0;
    vfs_node_t*  current;

    if (!path) return 0;

    current = vfs_find_mount(path, &rest);
    if (!current) return 0;

    /* Walk each path component */
    while (rest && *rest != '\0') {
        char         component[VFS_NAME_MAX];
        unsigned int k = 0;
        vfs_node_t*  next;

        /* Extract next component */
        while (rest[k] != '\0' && rest[k] != '/' && k < VFS_NAME_MAX - 1U)
            component[k++] = rest[k];
        component[k] = '\0';
        rest += k;
        if (*rest == '/') rest++;
        if (k == 0) continue; /* skip double slashes */

        next = vfs_node_finddir(current, component);

        /* Free intermediate dynamically-allocated nodes */
        if (current->flags & VFS_FLAG_DYNAMIC)
            kfree(current);

        if (!next) return 0;
        current = next;
    }

    return current;
}

/* ============================================================
 *  File-descriptor API
 * ============================================================ */

int vfs_open(const char* path) {
    vfs_node_t* node;
    int i;

    node = vfs_resolve(path);
    if (!node) return -1;

    /* Call open hook if provided */
    if (node->ops && node->ops->open) {
        if (node->ops->open(node) < 0) {
            if (node->flags & VFS_FLAG_DYNAMIC) kfree(node);
            return -1;
        }
    }

    /* Allocate a free file-descriptor slot */
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!fd_table[i].used) {
            fd_table[i].node   = node;
            fd_table[i].offset = 0;
            fd_table[i].used   = 1;
            return i;
        }
    }

    /* No free slots */
    if (node->flags & VFS_FLAG_DYNAMIC) kfree(node);
    return -1;
}

void vfs_close(int fd) {
    vfs_node_t* node;

    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].used) return;

    node = fd_table[fd].node;
    if (node) {
        if (node->ops && node->ops->close)
            node->ops->close(node);
        if (node->flags & VFS_FLAG_DYNAMIC)
            kfree(node);
    }

    fd_table[fd].node   = 0;
    fd_table[fd].offset = 0;
    fd_table[fd].used   = 0;
}

int vfs_read(int fd, unsigned char* buf, unsigned int size) {
    int result;

    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].used) return -1;
    if (!buf || size == 0) return 0;

    result = vfs_node_read(fd_table[fd].node, fd_table[fd].offset, size, buf);
    if (result > 0)
        fd_table[fd].offset += (unsigned int)result;

    return result;
}

int vfs_write(int fd, const unsigned char* buf, unsigned int size) {
    int result;

    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].used) return -1;
    if (!buf || size == 0) return 0;

    result = vfs_node_write(fd_table[fd].node, fd_table[fd].offset, size, buf);
    if (result > 0) {
        fd_table[fd].offset += (unsigned int)result;
        /* Refresh cached size after write */
        if (fd_table[fd].node)
            fd_table[fd].node->size = fd_table[fd].offset > fd_table[fd].node->size
                                       ? fd_table[fd].offset
                                       : fd_table[fd].node->size;
    }

    return result;
}

int vfs_seek(int fd, int offset, int whence) {
    unsigned int new_offset;
    vfs_node_t*  node;

    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].used) return -1;
    node = fd_table[fd].node;

    switch (whence) {
        case VFS_SEEK_SET:
            if (offset < 0) return -1;
            new_offset = (unsigned int)offset;
            break;
        case VFS_SEEK_CUR:
            new_offset = (unsigned int)((int)fd_table[fd].offset + offset);
            break;
        case VFS_SEEK_END:
            if (!node) return -1;
            new_offset = (unsigned int)((int)node->size + offset);
            break;
        default:
            return -1;
    }

    fd_table[fd].offset = new_offset;
    return (int)new_offset;
}

int vfs_readdir(int fd, unsigned int index, char* name_out, unsigned int name_max) {
    if (fd < 0 || fd >= VFS_MAX_FD || !fd_table[fd].used) return -1;
    return vfs_node_readdir(fd_table[fd].node, index, name_out, name_max);
}

/* ---- FD introspection ---- */

int vfs_fd_valid(int fd) {
    return fd >= 0 && fd < VFS_MAX_FD && fd_table[fd].used;
}

vfs_node_t* vfs_fd_node(int fd) {
    if (!vfs_fd_valid(fd)) return 0;
    return fd_table[fd].node;
}

unsigned int vfs_fd_offset(int fd) {
    if (!vfs_fd_valid(fd)) return 0;
    return fd_table[fd].offset;
}
