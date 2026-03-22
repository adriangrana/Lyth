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
 *  UNIX-like permission metadata (path -> mode bits)
 * ============================================================ */

#define VFS_PERM_MAX 256

typedef struct {
    int          used;
    char         path[VFS_PATH_MAX];
    unsigned int mode;
    unsigned int uid;
    unsigned int gid;
} vfs_perm_entry_t;

static vfs_perm_entry_t vfs_perm_table[VFS_PERM_MAX];

static void vfs_perm_init(void) {
    int i;
    for (i = 0; i < VFS_PERM_MAX; i++) {
        vfs_perm_table[i].used = 0;
        vfs_perm_table[i].path[0] = '\0';
        vfs_perm_table[i].mode = 0;
        vfs_perm_table[i].uid = 0;
        vfs_perm_table[i].gid = 0;
    }
}

/* ============================================================
 *  Vnode / dentry caches (fsck-lite style, conservative)
 *
 *  - vnode cache: path -> node for fast repeated open(path)
 *  - dentry cache: (parent,name) -> child/static-hit or negative-miss
 *
 *  Notes:
 *  - Positive dentry hits are cached only for STATIC nodes.
 *  - Dynamic nodes are not cached in dentry cache (ownership complexity).
 *  - Namespace mutations invalidate both caches.
 * ============================================================ */

#define VFS_VNODE_CACHE_MAX 64
#define VFS_DENTRY_CACHE_MAX 128

typedef struct {
    int         used;
    unsigned int age;
    char        path[VFS_PATH_MAX];
    vfs_node_t* node;
} vfs_vnode_cache_entry_t;

typedef struct {
    int         used;
    unsigned int age;
    unsigned int gen;
    vfs_node_t* parent;
    char        name[VFS_NAME_MAX];
    vfs_node_t* node;      /* valid when negative == 0 */
    int         negative;  /* 1 => cached miss */
} vfs_dentry_cache_entry_t;

static vfs_vnode_cache_entry_t vnode_cache[VFS_VNODE_CACHE_MAX];
static vfs_dentry_cache_entry_t dentry_cache[VFS_DENTRY_CACHE_MAX];
static unsigned int vnode_cache_tick = 1;
static unsigned int dentry_cache_tick = 1;
static unsigned int dentry_cache_gen = 1;

static void vfs_copy_str_bounded(char* dst, unsigned int dst_max, const char* src) {
    unsigned int i = 0;
    if (!dst || dst_max == 0 || !src) return;
    while (src[i] != '\0' && i + 1U < dst_max) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

static void vfs_vnode_cache_init(void) {
    int i;
    for (i = 0; i < VFS_VNODE_CACHE_MAX; i++) {
        vnode_cache[i].used = 0;
        vnode_cache[i].age = 0;
        vnode_cache[i].path[0] = '\0';
        vnode_cache[i].node = 0;
    }
}

static void vfs_dentry_cache_init(void) {
    int i;
    for (i = 0; i < VFS_DENTRY_CACHE_MAX; i++) {
        dentry_cache[i].used = 0;
        dentry_cache[i].age = 0;
        dentry_cache[i].gen = 0;
        dentry_cache[i].parent = 0;
        dentry_cache[i].name[0] = '\0';
        dentry_cache[i].node = 0;
        dentry_cache[i].negative = 0;
    }
}

static void vfs_cache_invalidate_all(void) {
    vfs_vnode_cache_init();
    dentry_cache_gen++;
    if (dentry_cache_gen == 0U) dentry_cache_gen = 1U;
    vfs_dentry_cache_init();
}

static void vfs_vnode_cache_remove_node(vfs_node_t* node) {
    int i;
    if (!node) return;
    for (i = 0; i < VFS_VNODE_CACHE_MAX; i++) {
        if (vnode_cache[i].used && vnode_cache[i].node == node) {
            vnode_cache[i].used = 0;
            vnode_cache[i].node = 0;
            vnode_cache[i].path[0] = '\0';
            vnode_cache[i].age = 0;
        }
    }
}

static void vfs_vnode_cache_insert(const char* path, vfs_node_t* node) {
    int i;
    int slot = -1;
    unsigned int best_age = 0xFFFFFFFFU;

    if (!path || !node || path[0] == '\0') return;

    for (i = 0; i < VFS_VNODE_CACHE_MAX; i++) {
        if (vnode_cache[i].used && str_equals(vnode_cache[i].path, path)) {
            vnode_cache[i].node = node;
            vnode_cache[i].age = vnode_cache_tick++;
            if (vnode_cache_tick == 0U) vnode_cache_tick = 1U;
            return;
        }
    }

    for (i = 0; i < VFS_VNODE_CACHE_MAX; i++) {
        if (!vnode_cache[i].used) { slot = i; break; }
        if (vnode_cache[i].age < best_age) {
            best_age = vnode_cache[i].age;
            slot = i;
        }
    }

    if (slot < 0) return;
    vnode_cache[slot].used = 1;
    vnode_cache[slot].node = node;
    vfs_copy_str_bounded(vnode_cache[slot].path, VFS_PATH_MAX, path);
    vnode_cache[slot].age = vnode_cache_tick++;
    if (vnode_cache_tick == 0U) vnode_cache_tick = 1U;
}

static vfs_node_t* vfs_vnode_cache_lookup(const char* path) {
    int i;
    if (!path) return 0;
    for (i = 0; i < VFS_VNODE_CACHE_MAX; i++) {
        if (!vnode_cache[i].used) continue;
        if (str_equals(vnode_cache[i].path, path)) {
            vnode_cache[i].age = vnode_cache_tick++;
            if (vnode_cache_tick == 0U) vnode_cache_tick = 1U;
            return vnode_cache[i].node;
        }
    }
    return 0;
}

static int vfs_dentry_cache_lookup(vfs_node_t* parent, const char* name,
                                   vfs_node_t** out_node, int* out_negative) {
    int i;
    if (!parent || !name || !out_node || !out_negative) return 0;
    for (i = 0; i < VFS_DENTRY_CACHE_MAX; i++) {
        if (!dentry_cache[i].used) continue;
        if (dentry_cache[i].gen != dentry_cache_gen) continue;
        if (dentry_cache[i].parent != parent) continue;
        if (!str_equals(dentry_cache[i].name, name)) continue;

        dentry_cache[i].age = dentry_cache_tick++;
        if (dentry_cache_tick == 0U) dentry_cache_tick = 1U;
        *out_negative = dentry_cache[i].negative;
        *out_node = dentry_cache[i].node;
        return 1;
    }
    return 0;
}

static void vfs_dentry_cache_insert(vfs_node_t* parent, const char* name,
                                    vfs_node_t* node, int negative) {
    int i;
    int slot = -1;
    unsigned int best_age = 0xFFFFFFFFU;

    if (!parent || !name || name[0] == '\0') return;

    for (i = 0; i < VFS_DENTRY_CACHE_MAX; i++) {
        if (!dentry_cache[i].used) continue;
        if (dentry_cache[i].gen != dentry_cache_gen) continue;
        if (dentry_cache[i].parent == parent && str_equals(dentry_cache[i].name, name)) {
            dentry_cache[i].node = node;
            dentry_cache[i].negative = negative ? 1 : 0;
            dentry_cache[i].age = dentry_cache_tick++;
            if (dentry_cache_tick == 0U) dentry_cache_tick = 1U;
            return;
        }
    }

    for (i = 0; i < VFS_DENTRY_CACHE_MAX; i++) {
        if (!dentry_cache[i].used) { slot = i; break; }
        if (dentry_cache[i].gen != dentry_cache_gen) { slot = i; break; }
        if (dentry_cache[i].age < best_age) {
            best_age = dentry_cache[i].age;
            slot = i;
        }
    }
    if (slot < 0) return;

    dentry_cache[slot].used = 1;
    dentry_cache[slot].gen = dentry_cache_gen;
    dentry_cache[slot].parent = parent;
    vfs_copy_str_bounded(dentry_cache[slot].name, VFS_NAME_MAX, name);
    dentry_cache[slot].node = node;
    dentry_cache[slot].negative = negative ? 1 : 0;
    dentry_cache[slot].age = dentry_cache_tick++;
    if (dentry_cache_tick == 0U) dentry_cache_tick = 1U;
}

/* ============================================================
 *  File-descriptor tables
 *  Each task owns its own fd_table (embedded in task_entry_t).
 *  The kernel fallback is used when no task is running (boot/IRQ context).
 * ============================================================ */

static vfs_fd_entry_t kernel_fd_table[VFS_MAX_FD];
static void vfs_node_ref(vfs_node_t* node);
static int vfs_perm_set(const char* path, unsigned int mode, unsigned int uid, unsigned int gid);
static unsigned int vfs_default_mode_for_path(const char* path, unsigned int flags);
static int vfs_perm_get_owner(const char* path, unsigned int* uid_out, unsigned int* gid_out);

/* Global TTY node for stdin/stdout/stderr pre-installation. */
static vfs_node_t* g_tty_node = 0;

void vfs_set_tty_node(vfs_node_t* node) {
    g_tty_node = node;
}

void vfs_install_stdio(vfs_fd_entry_t* table) {
    int i;
    static const unsigned int stdio_flags[3] = {
        VFS_O_RDONLY,
        VFS_O_WRONLY,
        VFS_O_WRONLY
    };
    if (!table || !g_tty_node) return;
    for (i = 0; i < 3; i++) {
        if (!table[i].used) {
            vfs_node_ref(g_tty_node);
            table[i].node   = g_tty_node;
            table[i].offset = 0;
            table[i].open_flags = stdio_flags[i];
            table[i].used   = 1;
        }
    }
}

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
    vfs_perm_init();
    vfs_vnode_cache_init();
    vfs_dentry_cache_init();
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
            vfs_perm_set(mounts[i].path,
                         vfs_default_mode_for_path(mounts[i].path, root->flags),
                         0U,
                         0U);
            vfs_cache_invalidate_all();
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
        int          negative = 0;
        vfs_node_t*  cached = 0;

        /* Extract next component */
        while (rest[k] != '\0' && rest[k] != '/' && k < VFS_NAME_MAX - 1U)
            component[k++] = rest[k];
        component[k] = '\0';
        rest += k;
        if (*rest == '/') rest++;
        if (k == 0) continue; /* skip double slashes */

        if (vfs_dentry_cache_lookup(current, component, &cached, &negative)) {
            if (negative) {
                if (current->flags & VFS_FLAG_DYNAMIC)
                    kfree(current);
                return 0;
            }
            next = cached;
        } else {
            next = vfs_node_finddir(current, component);
            if (!next) {
                vfs_dentry_cache_insert(current, component, 0, 1);
            } else if ((next->flags & VFS_FLAG_DYNAMIC) == 0U) {
                vfs_dentry_cache_insert(current, component, next, 0);
            }
        }

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

static int vfs_perm_find(const char* path) {
    int i;
    if (!path) return -1;
    for (i = 0; i < VFS_PERM_MAX; i++) {
        if (!vfs_perm_table[i].used) continue;
        if (str_equals(vfs_perm_table[i].path, path)) return i;
    }
    return -1;
}

static int vfs_perm_set(const char* path, unsigned int mode, unsigned int uid, unsigned int gid) {
    int i = vfs_perm_find(path);
    unsigned int j;

    mode &= 0x01FFU;

    if (i >= 0) {
        vfs_perm_table[i].mode = mode;
        vfs_perm_table[i].uid = uid;
        vfs_perm_table[i].gid = gid;
        return 0;
    }

    for (i = 0; i < VFS_PERM_MAX; i++) {
        if (!vfs_perm_table[i].used) {
            vfs_perm_table[i].used = 1;
            for (j = 0; path[j] && j < VFS_PATH_MAX - 1U; j++)
                vfs_perm_table[i].path[j] = path[j];
            vfs_perm_table[i].path[j] = '\0';
            vfs_perm_table[i].mode = mode;
            vfs_perm_table[i].uid = uid;
            vfs_perm_table[i].gid = gid;
            return 0;
        }
    }

    return -1;
}

static void vfs_perm_remove(const char* path) {
    int i = vfs_perm_find(path);
    if (i < 0) return;
    vfs_perm_table[i].used = 0;
    vfs_perm_table[i].path[0] = '\0';
    vfs_perm_table[i].mode = 0;
    vfs_perm_table[i].uid = 0;
    vfs_perm_table[i].gid = 0;
}

static unsigned int vfs_default_mode_for_path(const char* path, unsigned int flags) {
    if (path && str_equals(path, "/")) return VFS_MODE_ROOT_DEFAULT;
    if (flags & VFS_FLAG_DIR) return VFS_MODE_DIR_DEFAULT;
    return VFS_MODE_FILE_DEFAULT;
}

static int vfs_mode_check_read(unsigned int mode, unsigned int owner_uid, unsigned int owner_gid) {
    unsigned int euid = task_current_euid();
    if (euid == 0U) return 1; /* root bypass */
    if (euid == owner_uid) return (mode & VFS_MODE_IRUSR) ? 1 : 0;
    if (task_in_group(owner_gid)) return (mode & VFS_MODE_IRGRP) ? 1 : 0;
    return (mode & VFS_MODE_IROTH) ? 1 : 0;
}

static int vfs_mode_check_write(unsigned int mode, unsigned int owner_uid, unsigned int owner_gid) {
    unsigned int euid = task_current_euid();
    if (euid == 0U) return 1;
    if (euid == owner_uid) return (mode & VFS_MODE_IWUSR) ? 1 : 0;
    if (task_in_group(owner_gid)) return (mode & VFS_MODE_IWGRP) ? 1 : 0;
    return (mode & VFS_MODE_IWOTH) ? 1 : 0;
}

static int vfs_mode_check_exec(unsigned int mode, unsigned int owner_uid, unsigned int owner_gid) {
    unsigned int euid = task_current_euid();
    if (euid == 0U) return 1;
    if (euid == owner_uid) return (mode & VFS_MODE_IXUSR) ? 1 : 0;
    if (task_in_group(owner_gid)) return (mode & VFS_MODE_IXGRP) ? 1 : 0;
    return (mode & VFS_MODE_IXOTH) ? 1 : 0;
}

static int vfs_perm_get_owner(const char* path, unsigned int* uid_out, unsigned int* gid_out) {
    int i;
    if (!path || !uid_out || !gid_out) return -1;
    i = vfs_perm_find(path);
    if (i < 0) return -1;
    *uid_out = vfs_perm_table[i].uid;
    *gid_out = vfs_perm_table[i].gid;
    return 0;
}

int vfs_chmod(const char* path, unsigned int mode) {
    vfs_node_t* node;
    int dynamic_node;
    unsigned int uid = 0U;
    unsigned int gid = 0U;

    if (!path) return -1;
    node = vfs_resolve(path);
    if (!node) return -1;

    dynamic_node = (int)(node->flags & VFS_FLAG_DYNAMIC);
    if (vfs_perm_get_owner(path, &uid, &gid) != 0) {
        uid = 0U;
        gid = 0U;
    }
    /* chmod allowed for root or owner */
    if (task_current_euid() != 0U && task_current_euid() != uid) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }
    if (vfs_perm_set(path, mode & 0x01FFU, uid, gid) != 0) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if (dynamic_node && node->ref_count == 0U) kfree(node);
    return 0;
}

int vfs_get_mode(const char* path, unsigned int* mode_out) {
    int i;
    vfs_node_t* node;
    int dynamic_node;
    unsigned int mode;

    if (!path || !mode_out) return -1;

    i = vfs_perm_find(path);
    if (i >= 0) {
        *mode_out = vfs_perm_table[i].mode;
        return 0;
    }

    node = vfs_resolve(path);
    if (!node) return -1;
    dynamic_node = (int)(node->flags & VFS_FLAG_DYNAMIC);

    mode = vfs_default_mode_for_path(path, node->flags);
    if (vfs_perm_set(path, mode, 0U, 0U) != 0) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if (dynamic_node && node->ref_count == 0U) kfree(node);
    *mode_out = mode;
    return 0;
}

int vfs_chown(const char* path, unsigned int uid, unsigned int gid) {
    int i;
    /* Only root can chown/chgrp */
    if (task_current_euid() != 0U) return -1;
    if (!path) return -1;
    i = vfs_perm_find(path);
    if (i < 0) {
        unsigned int mode;
        if (vfs_get_mode(path, &mode) != 0) return -1;
        i = vfs_perm_find(path);
        if (i < 0) return -1;
    }
    vfs_perm_table[i].uid = uid;
    vfs_perm_table[i].gid = gid;
    return 0;
}

int vfs_get_owner(const char* path, unsigned int* uid_out, unsigned int* gid_out) {
    int i;
    if (!path || !uid_out || !gid_out) return -1;
    i = vfs_perm_find(path);
    if (i < 0) {
        unsigned int mode;
        if (vfs_get_mode(path, &mode) != 0) return -1;
        i = vfs_perm_find(path);
        if (i < 0) return -1;
    }
    *uid_out = vfs_perm_table[i].uid;
    *gid_out = vfs_perm_table[i].gid;
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

    {
        unsigned int dir_mode;
        unsigned int dir_uid = 0U;
        unsigned int dir_gid = 0U;
        if (vfs_get_mode(dir_buf, &dir_mode) != 0) return -1;
        if (vfs_get_owner(dir_buf, &dir_uid, &dir_gid) != 0) return -1;
        if (!vfs_mode_check_write(dir_mode, dir_uid, dir_gid) || !vfs_mode_check_exec(dir_mode, dir_uid, dir_gid))
            return -1;
    }

    dir = vfs_resolve(dir_buf);
    if (!dir) return -1;
    dynamic_dir = (int)(dir->flags & VFS_FLAG_DYNAMIC);

    result = vfs_node_create(dir, name_buf, flags);
    if (dynamic_dir) kfree(dir);

    if (!result) return -1;
    vfs_perm_set(path,
                 (flags & VFS_FLAG_DIR) ? VFS_MODE_DIR_DEFAULT : VFS_MODE_FILE_DEFAULT,
                 task_current_euid(),
                 task_current_egid());
    if (result->flags & VFS_FLAG_DYNAMIC) kfree(result);
    vfs_cache_invalidate_all();
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

    {
        unsigned int dir_mode;
        unsigned int dir_uid = 0U;
        unsigned int dir_gid = 0U;
        if (vfs_get_mode(dir_buf, &dir_mode) != 0) return -1;
        if (vfs_get_owner(dir_buf, &dir_uid, &dir_gid) != 0) return -1;
        if (!vfs_mode_check_write(dir_mode, dir_uid, dir_gid) || !vfs_mode_check_exec(dir_mode, dir_uid, dir_gid))
            return -1;
    }

    dir = vfs_resolve(dir_buf);
    if (!dir) return -1;
    dynamic_dir = (int)(dir->flags & VFS_FLAG_DYNAMIC);

    result = vfs_node_unlink(dir, name_buf);
    if (dynamic_dir) kfree(dir);
    if (result == 0) {
        vfs_perm_remove(path);
        vfs_cache_invalidate_all();
    }
    return result;
}

int vfs_unlink(const char* path) {
    return vfs_delete(path);
}

int vfs_stat(const char* path, vfs_stat_t* out) {
    vfs_node_t* node;
    int         dynamic_node;

    if (!path || !out) return -1;

    node = vfs_resolve(path);
    if (!node) return -1;

    dynamic_node = (int)(node->flags & VFS_FLAG_DYNAMIC);
    out->size  = node->size;
    out->flags = node->flags;
    if (vfs_get_mode(path, &out->mode) != 0)
        out->mode = vfs_default_mode_for_path(path, node->flags);
    if (vfs_get_owner(path, &out->uid, &out->gid) != 0) {
        out->uid = 0U;
        out->gid = 0U;
    }

    if (dynamic_node && node->ref_count == 0U) kfree(node);
    return 0;
}

int vfs_rename(const char* old_path, const char* new_path) {
    int            src_fd;
    int            dst_fd;
    unsigned char  buf[512];
    int            copied_any = 0;
    unsigned int   old_mode = 0;
    char           old_dir[VFS_PATH_MAX];
    char           old_name[VFS_NAME_MAX];
    char           new_dir[VFS_PATH_MAX];
    char           new_name[VFS_NAME_MAX];

    if (!old_path || !new_path) return -1;
    if (str_equals(old_path, new_path)) return 0;

    if (split_path(old_path, old_dir, VFS_PATH_MAX, old_name, VFS_NAME_MAX) != 0) return -1;
    if (split_path(new_path, new_dir, VFS_PATH_MAX, new_name, VFS_NAME_MAX) != 0) return -1;
    {
        unsigned int m;
        unsigned int duid = 0U;
        unsigned int dgid = 0U;
        if (vfs_get_mode(old_dir, &m) != 0) return -1;
        if (vfs_get_owner(old_dir, &duid, &dgid) != 0) return -1;
        if (!vfs_mode_check_write(m, duid, dgid) || !vfs_mode_check_exec(m, duid, dgid)) return -1;
        if (vfs_get_mode(new_dir, &m) != 0) return -1;
        if (vfs_get_owner(new_dir, &duid, &dgid) != 0) return -1;
        if (!vfs_mode_check_write(m, duid, dgid) || !vfs_mode_check_exec(m, duid, dgid)) return -1;
    }

    (void)vfs_get_mode(old_path, &old_mode);

    src_fd = vfs_open(old_path);
    if (src_fd < 0) return -1;

    if (vfs_fd_node(src_fd) && (vfs_fd_node(src_fd)->flags & VFS_FLAG_DIR)) {
        vfs_close(src_fd);
        return -1;
    }

    if (vfs_create(new_path, VFS_FLAG_FILE) != 0) {
        if (vfs_delete(new_path) != 0 || vfs_create(new_path, VFS_FLAG_FILE) != 0) {
            vfs_close(src_fd);
            return -1;
        }
    }

    dst_fd = vfs_open(new_path);
    if (dst_fd < 0) {
        vfs_close(src_fd);
        return -1;
    }

    while (1) {
        int n = vfs_read(src_fd, buf, sizeof(buf));
        if (n < 0) {
            vfs_close(dst_fd);
            vfs_close(src_fd);
            return -1;
        }
        if (n == 0) break;
        copied_any = 1;
        if (vfs_write(dst_fd, buf, (unsigned int)n) != n) {
            vfs_close(dst_fd);
            vfs_close(src_fd);
            return -1;
        }
    }

    vfs_close(dst_fd);
    vfs_close(src_fd);

    if (!copied_any) {
        int dst_open = vfs_open(new_path);
        if (dst_open >= 0) vfs_close(dst_open);
    }

    if (vfs_delete(old_path) != 0) return -1;
    if (old_mode != 0U) {
        unsigned int old_uid = 0U;
        unsigned int old_gid = 0U;
        if (vfs_perm_get_owner(old_path, &old_uid, &old_gid) != 0) {
            old_uid = 0U;
            old_gid = 0U;
        }
        vfs_perm_set(new_path, old_mode, old_uid, old_gid);
    }
    vfs_cache_invalidate_all();
    return 0;
}

static unsigned int vfs_open_sanitize_flags(unsigned int open_flags) {
    if ((open_flags & VFS_O_ACCMODE) == 0)
        open_flags |= VFS_O_RDWR;
    return open_flags;
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
        vfs_vnode_cache_remove_node(node);
        if (node->ops && node->ops->close)
            node->ops->close(node);
        if (node->flags & VFS_FLAG_DYNAMIC)
            kfree(node);
    }
}

int vfs_open_flags(const char* path, unsigned int open_flags) {
    vfs_fd_entry_t* fdt;
    vfs_node_t* node;
    int dynamic_node;
    unsigned int mode = 0;
    unsigned int owner_uid = 0U;
    unsigned int owner_gid = 0U;
    int need_read;
    int need_write;
    int need_exec;
    int i;

    if (!path) return -1;

    open_flags = vfs_open_sanitize_flags(open_flags);
    node = vfs_vnode_cache_lookup(path);
    if (!node)
        node = vfs_resolve(path);

    if (node && (open_flags & VFS_O_CREAT) && (open_flags & VFS_O_EXCL)) {
        if ((node->flags & VFS_FLAG_DYNAMIC) && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if (!node && (open_flags & VFS_O_CREAT)) {
        if (vfs_create(path, VFS_FLAG_FILE) != 0)
            return -1;
        node = vfs_resolve(path);
    }
    if (!node) return -1;

    dynamic_node = (int)(node->flags & VFS_FLAG_DYNAMIC);

    if (vfs_get_mode(path, &mode) != 0) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }
    if (vfs_get_owner(path, &owner_uid, &owner_gid) != 0) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    need_read  = ((open_flags & VFS_O_RDONLY) != 0U) ? 1 : 0;
    need_write = ((open_flags & VFS_O_WRONLY) != 0U) ? 1 : 0;
    if (open_flags & (VFS_O_TRUNC | VFS_O_APPEND)) need_write = 1;
    need_exec  = (node->flags & VFS_FLAG_DIR) ? 1 : 0;

    if (need_read && !vfs_mode_check_read(mode, owner_uid, owner_gid)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }
    if (need_write && !vfs_mode_check_write(mode, owner_uid, owner_gid)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }
    if (need_exec && !vfs_mode_check_exec(mode, owner_uid, owner_gid)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if ((open_flags & VFS_O_DIRECTORY) && !(node->flags & VFS_FLAG_DIR)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if ((node->flags & VFS_FLAG_DIR) &&
        ((open_flags & VFS_O_ACCMODE) == VFS_O_WRONLY)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        return -1;
    }

    if ((open_flags & VFS_O_TRUNC) &&
        (open_flags & VFS_O_WRONLY) &&
        (node->flags & VFS_FLAG_FILE)) {
        if (dynamic_node && node->ref_count == 0U) kfree(node);
        if (vfs_delete(path) != 0) return -1;
        if (vfs_create(path, VFS_FLAG_FILE) != 0) return -1;
        node = vfs_resolve(path);
        if (!node) return -1;
        dynamic_node = (int)(node->flags & VFS_FLAG_DYNAMIC);
    }

    /* Call open hook if provided */
    if (node->ops && node->ops->open) {
        if (node->ops->open(node) < 0) {
            if (dynamic_node && node->ref_count == 0U) kfree(node);
            return -1;
        }
    }

    /* Initialise / increment reference count.
     * DYNAMIC nodes are freshly kmalloc'd for each open, so we own them
     * exclusively and set ref_count = 1.  Static (embedded) nodes may
     * already have open references, so we just increment. */
    if (node->flags & VFS_FLAG_DYNAMIC) {
        if (node->ref_count == 0U)
            node->ref_count = 1;
        else
            vfs_node_ref(node);
    } else
        vfs_node_ref(node);

    vfs_vnode_cache_insert(path, node);

    fdt = vfs_get_current_fd_table();

    /* Allocate a free file-descriptor slot */
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!fdt[i].used) {
            fdt[i].node   = node;
            fdt[i].offset = (open_flags & VFS_O_APPEND) ? node->size : 0U;
            fdt[i].open_flags = open_flags;
            fdt[i].used   = 1;
            return i;
        }
    }

    /* No free slots — release the reference we just took */
    vfs_node_unref(node);
    return -1;
}

int vfs_open(const char* path) {
    return vfs_open_flags(path, VFS_O_RDWR);
}

int vfs_fd_install_node(vfs_node_t* node, unsigned int open_flags) {
    vfs_fd_entry_t* fdt;
    int i;

    if (node == 0) return -1;

    if (node->flags & VFS_FLAG_DYNAMIC) {
        if (node->ref_count == 0U)
            node->ref_count = 1U;
        else
            vfs_node_ref(node);
    } else {
        vfs_node_ref(node);
    }

    fdt = vfs_get_current_fd_table();
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!fdt[i].used) {
            fdt[i].node = node;
            fdt[i].offset = 0U;
            fdt[i].open_flags = open_flags;
            fdt[i].used = 1;
            return i;
        }
    }

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
    fdt[fd].open_flags = 0;
    fdt[fd].used   = 0;
}

int vfs_read(int fd, unsigned char* buf, unsigned int size) {
    vfs_fd_entry_t* fdt;
    int result;

    if (fd < 0 || fd >= VFS_MAX_FD) return -1;
    if (!buf || size == 0) return 0;

    fdt = vfs_get_current_fd_table();
    if (!fdt[fd].used) return -1;
    if ((fdt[fd].open_flags & VFS_O_RDONLY) == 0) return -1;

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
    if ((fdt[fd].open_flags & VFS_O_WRONLY) == 0) return -1;

    if ((fdt[fd].open_flags & VFS_O_APPEND) && fdt[fd].node)
        fdt[fd].offset = fdt[fd].node->size;

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
        table[i].open_flags = 0;
        table[i].used   = 0;
    }
    /* Pre-install TTY on stdin/stdout/stderr if available. */
    vfs_install_stdio(table);
}

void vfs_task_fd_close_all(vfs_fd_entry_t* table) {
    int i;
    if (!table) return;
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (!table[i].used) continue;
        vfs_node_unref(table[i].node);  /* close + conditional free */
        table[i].node   = 0;
        table[i].offset = 0;
        table[i].open_flags = 0;
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
