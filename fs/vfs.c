#include "vfs.h"
#include "heap.h"
#include "string.h"
#include "task.h"

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
 *  File-descriptor tables
 *  Each task owns its own fd_table (embedded in task_entry_t).
 *  The kernel fallback is used when no task is running (boot/IRQ context).
 * ============================================================ */

static vfs_fd_entry_t kernel_fd_table[VFS_MAX_FD];

/* Return the fd table that belongs to the currently running context. */
static vfs_fd_entry_t* vfs_get_current_fd_table(void) {
    if (task_is_running()) {
        vfs_fd_entry_t* t = task_current_fd_table();
        if (t) return t;
    }
    return kernel_fd_table;
}

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
        kernel_fd_table[i].used   = 0;
        kernel_fd_table[i].node   = 0;
        kernel_fd_table[i].offset = 0;
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

vfs_node_t* vfs_node_create(vfs_node_t* dir, const char* name, unsigned int flags) {
    vfs_node_t* n;
    if (!dir || !name) return 0;
    n = vfs_effective(dir);
    if (!n->ops || !n->ops->create) return 0;
    return n->ops->create(n, name, flags);
}

int vfs_node_unlink(vfs_node_t* dir, const char* name) {
    vfs_node_t* n;
    if (!dir || !name) return -1;
    n = vfs_effective(dir);
    if (!n->ops || !n->ops->unlink) return -1;
    return n->ops->unlink(n, name);
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
 *  Create / Delete
 * ============================================================ */

/*
 * Split an absolute path into parent-directory and final component.
 * "/a/b/c" -> dir="/a/b", name="c"
 * "/foo"   -> dir="/",    name="foo"
 * Returns 0 on success, -1 on malformed input.
 */
static int split_path(const char* path,
                      char* dir_out, unsigned int dir_max,
                      char* name_out, unsigned int name_max) {
    unsigned int len;
    int          last_slash = -1;
    unsigned int i;

    if (!path || path[0] != '/') return -1;

    for (len = 0; path[len] != '\0'; len++);
    /* Strip trailing slash */
    while (len > 1 && path[len - 1] == '/') len--;
    if (len == 0) return -1;

    for (i = 0; i < len; i++) {
        if (path[i] == '/') last_slash = (int)i;
    }
    if (last_slash < 0) return -1;

    /* Name component */
    {
        const char*  src = path + last_slash + 1;
        unsigned int k;
        if (src[0] == '\0') return -1; /* e.g. "/" has no file part */
        for (k = 0; src[k] != '\0' && k < name_max - 1U; k++)
            name_out[k] = src[k];
        name_out[k] = '\0';
    }

    /* Dir component */
    if (last_slash == 0) {
        if (dir_max < 2U) return -1;
        dir_out[0] = '/'; dir_out[1] = '\0';
    } else {
        unsigned int dir_len = (unsigned int)last_slash;
        if (dir_len >= dir_max) dir_len = dir_max - 1U;
        for (i = 0; i < dir_len; i++) dir_out[i] = path[i];
        dir_out[dir_len] = '\0';
    }

    return 0;
}

int vfs_create(const char* path, unsigned int flags) {
    char        dir_buf[VFS_PATH_MAX];
    char        name_buf[VFS_NAME_MAX];
    vfs_node_t* dir;
    vfs_node_t* result;
    int         dynamic_dir;

    if (!path) return -1;
    if (split_path(path, dir_buf, VFS_PATH_MAX, name_buf, VFS_NAME_MAX) < 0)
        return -1;

    dir = vfs_resolve(dir_buf);
    if (!dir) return -1;
    dynamic_dir = (int)(dir->flags & VFS_FLAG_DYNAMIC);

    result = vfs_node_create(dir, name_buf, flags);
    if (dynamic_dir) kfree(dir);

    if (!result) return -1;
    if (result->flags & VFS_FLAG_DYNAMIC) kfree(result);
    return 0;
}

int vfs_delete(const char* path) {
    char        dir_buf[VFS_PATH_MAX];
    char        name_buf[VFS_NAME_MAX];
    vfs_node_t* dir;
    int         dynamic_dir;
    int         result;

    if (!path) return -1;
    if (split_path(path, dir_buf, VFS_PATH_MAX, name_buf, VFS_NAME_MAX) < 0)
        return -1;

    dir = vfs_resolve(dir_buf);
    if (!dir) return -1;
    dynamic_dir = (int)(dir->flags & VFS_FLAG_DYNAMIC);

    result = vfs_node_unlink(dir, name_buf);
    if (dynamic_dir) kfree(dir);
    return result;
}

/* ============================================================
 *  File-descriptor API
 * ============================================================ */

/* Increment the reference count on a node. */
static void vfs_node_ref(vfs_node_t* node) {
    if (node) node->ref_count++;
}

/* Decrement the reference count.  When it reaches zero, call the close
   vtable hook (once) and free the node if it is DYNAMIC-allocated. */
static void vfs_node_unref(vfs_node_t* node) {
    if (!node) return;
    if (node->ref_count > 0) node->ref_count--;
    if (node->ref_count == 0) {
        if (node->ops && node->ops->close)
            node->ops->close(node);
        if (node->flags & VFS_FLAG_DYNAMIC)
            kfree(node);
    }
}

int vfs_open(const char* path) {
    vfs_fd_entry_t* fdt;
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

    /* Initialise / increment reference count.
     * DYNAMIC nodes are freshly kmalloc'd for each open, so we own them
     * exclusively and set ref_count = 1.  Static (embedded) nodes may
     * already have open references, so we just increment. */
    if (node->flags & VFS_FLAG_DYNAMIC)
        node->ref_count = 1;
    else
        vfs_node_ref(node);

    fdt = vfs_get_current_fd_table();

    /* Allocate a free file-descriptor slot */
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!fdt[i].used) {
            fdt[i].node   = node;
            fdt[i].offset = 0;
            fdt[i].used   = 1;
            return i;
        }
    }

    /* No free slots — release the reference we just took */
    vfs_node_unref(node);
    return -1;
}

void vfs_close(int fd) {
    vfs_fd_entry_t* fdt;
    vfs_node_t* node;

    if (fd < 0 || fd >= VFS_MAX_FD) return;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return;

    node = fdt[fd].node;
    vfs_node_unref(node);   /* close hook + conditional free when last ref */

    fdt[fd].node   = 0;
    fdt[fd].offset = 0;
    fdt[fd].used   = 0;
}

int vfs_read(int fd, unsigned char* buf, unsigned int size) {
    vfs_fd_entry_t* fdt;
    int result;

    if (fd < 0 || fd >= VFS_MAX_FD) return -1;
    if (!buf || size == 0) return 0;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return -1;

    result = vfs_node_read(fdt[fd].node, fdt[fd].offset, size, buf);
    if (result > 0)
        fdt[fd].offset += (unsigned int)result;

    return result;
}

int vfs_write(int fd, const unsigned char* buf, unsigned int size) {
    vfs_fd_entry_t* fdt;
    int result;

    if (fd < 0 || fd >= VFS_MAX_FD) return -1;
    if (!buf || size == 0) return 0;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return -1;

    result = vfs_node_write(fdt[fd].node, fdt[fd].offset, size, buf);
    if (result > 0) {
        fdt[fd].offset += (unsigned int)result;
        /* Refresh cached size after write */
        if (fdt[fd].node)
            fdt[fd].node->size = fdt[fd].offset > fdt[fd].node->size
                                  ? fdt[fd].offset
                                  : fdt[fd].node->size;
    }

    return result;
}

int vfs_seek(int fd, int offset, int whence) {
    vfs_fd_entry_t* fdt;
    vfs_node_t*  node;
    unsigned int new_offset;

    if (fd < 0 || fd >= VFS_MAX_FD) return -1;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return -1;

    node = fdt[fd].node;

    switch (whence) {
        case VFS_SEEK_SET:
            if (offset < 0) return -1;
            new_offset = (unsigned int)offset;
            break;
        case VFS_SEEK_CUR:
            new_offset = (unsigned int)((int)fdt[fd].offset + offset);
            break;
        case VFS_SEEK_END:
            if (!node) return -1;
            new_offset = (unsigned int)((int)node->size + offset);
            break;
        default:
            return -1;
    }

    fdt[fd].offset = new_offset;
    return (int)new_offset;
}

int vfs_readdir(int fd, unsigned int index, char* name_out, unsigned int name_max) {
    vfs_fd_entry_t* fdt;

    if (fd < 0 || fd >= VFS_MAX_FD) return -1;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return -1;

    return vfs_node_readdir(fdt[fd].node, index, name_out, name_max);
}

/* ---- FD introspection ---- */

int vfs_fd_valid(int fd) {
    vfs_fd_entry_t* fdt;
    if (fd < 0 || fd >= VFS_MAX_FD) return 0;
    fdt = vfs_get_current_fd_table();
    return fdt[fd].used;
}

vfs_node_t* vfs_fd_node(int fd) {
    vfs_fd_entry_t* fdt;
    if (fd < 0 || fd >= VFS_MAX_FD) return 0;
    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return 0;
    return fdt[fd].node;
}

unsigned int vfs_fd_offset(int fd) {
    vfs_fd_entry_t* fdt;
    if (fd < 0 || fd >= VFS_MAX_FD) return 0;
    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return 0;
    return fdt[fd].offset;
}

/* ---- Per-task fd-table helpers (called from task.c) ---- */

void vfs_task_fd_init(vfs_fd_entry_t* table) {
    int i;
    if (!table) return;
    for (i = 0; i < VFS_MAX_FD; i++) {
        table[i].node   = 0;
        table[i].offset = 0;
        table[i].used   = 0;
    }
}

void vfs_task_fd_close_all(vfs_fd_entry_t* table) {
    int i;
    if (!table) return;
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!table[i].used) continue;
        vfs_node_unref(table[i].node);  /* close + conditional free */
        table[i].node   = 0;
        table[i].offset = 0;
        table[i].used   = 0;
    }
}

void vfs_task_fd_inherit(vfs_fd_entry_t* dst, const vfs_fd_entry_t* src) {
    int i;
    if (!dst || !src) return;
    for (i = 0; i < VFS_MAX_FD; i++) {
        dst[i] = src[i];                    /* copy node ptr, offset, used */
        if (dst[i].used && dst[i].node)
            vfs_node_ref(dst[i].node);      /* child holds an extra reference */
    }
}
