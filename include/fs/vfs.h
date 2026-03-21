#ifndef VFS_H
#define VFS_H

#include <stdint.h>

/* ---- Node flags ---- */
#define VFS_FLAG_FILE       0x01  /* regular file */
#define VFS_FLAG_DIR        0x02  /* directory */
#define VFS_FLAG_MOUNTPOINT 0x08  /* this node is a mountpoint */
#define VFS_FLAG_DYNAMIC    0x10  /* node was heap-allocated; free on close */

/* ---- Limits ---- */
#define VFS_NAME_MAX   64
#define VFS_PATH_MAX  256
#define VFS_MAX_FD     32
#define VFS_MAX_MOUNTS 16

/* ---- Seek whence constants ---- */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

/* ---- Open flags / access modes ---- */
#define VFS_O_RDONLY 0x0001U
#define VFS_O_WRONLY 0x0002U
#define VFS_O_RDWR   (VFS_O_RDONLY | VFS_O_WRONLY)
#define VFS_O_APPEND 0x0010U
#define VFS_O_CREAT  0x0020U
#define VFS_O_TRUNC  0x0040U
#define VFS_O_EXCL   0x0080U
#define VFS_O_DIRECTORY 0x0100U
#define VFS_O_NONBLOCK  0x0200U
#define VFS_O_ACCMODE (VFS_O_RDONLY | VFS_O_WRONLY)

/* ---- UNIX-like mode bits ---- */
#define VFS_MODE_IRUSR 0x0100U
#define VFS_MODE_IWUSR 0x0080U
#define VFS_MODE_IXUSR 0x0040U
#define VFS_MODE_IRGRP 0x0020U
#define VFS_MODE_IWGRP 0x0010U
#define VFS_MODE_IXGRP 0x0008U
#define VFS_MODE_IROTH 0x0004U
#define VFS_MODE_IWOTH 0x0002U
#define VFS_MODE_IXOTH 0x0001U

#define VFS_MODE_FILE_DEFAULT 0x01A4U /* 0644 */
#define VFS_MODE_DIR_DEFAULT  0x01EDU /* 0755 */
#define VFS_MODE_ROOT_DEFAULT 0x01FFU /* 0777 */

typedef struct vfs_node vfs_node_t;

typedef struct {
   unsigned int size;
   unsigned int flags;
   unsigned int mode;
} vfs_stat_t;

/* ---- Operations table (vtable for a filesystem backend) ---- */
typedef struct {
    /* Read 'size' bytes from 'offset' into 'buf'. Returns bytes read or -1. */
    int         (*read)   (vfs_node_t* node, unsigned int offset,
                           unsigned int size, unsigned char* buf);
    /* Write 'size' bytes from 'buf' at 'offset'. Returns bytes written or -1. */
    int         (*write)  (vfs_node_t* node, unsigned int offset,
                           unsigned int size, const unsigned char* buf);
    /* Enumerate entry 'index' in a directory. Copies name into name_out.
       Returns 0 on success, -1 when index is out of range. */
    int         (*readdir)(vfs_node_t* node, unsigned int index,
                           char* name_out, unsigned int name_max);
    /* Look up 'name' inside a directory. Returns a (possibly heap-allocated)
       node or NULL if not found. */
    vfs_node_t* (*finddir)(vfs_node_t* node, const char* name);
    /* Called when a file descriptor is opened. Returns 0 on success, -1 on error. */
    int         (*open)   (vfs_node_t* node);
    /* Called when a file descriptor is closed. */
    void        (*close)  (vfs_node_t* node);
    /* Create a new entry named 'name' inside this directory.
       'flags' is VFS_FLAG_FILE or VFS_FLAG_DIR.
       Returns a (possibly heap-allocated) node on success, NULL on error. */
    vfs_node_t* (*create) (vfs_node_t* dir, const char* name, unsigned int flags);
    /* Remove the entry 'name' from this directory.
       Returns 0 on success, -1 on error. */
    int         (*unlink) (vfs_node_t* dir, const char* name);
} vfs_ops_t;

/* ---- VFS node ---- */
struct vfs_node {
    char         name[VFS_NAME_MAX]; /* entry name (no leading path) */
    unsigned int flags;              /* VFS_FLAG_* */
    unsigned int size;               /* file size in bytes (0 for dirs) */
    unsigned int ref_count;          /* number of fd table entries pointing here */
    void*        impl;               /* backend private data */
    vfs_ops_t*   ops;                /* operation table */
    vfs_node_t*  mountpoint;        /* if non-NULL, redirect all ops here */
};

/* ---- Core VFS API ---- */

/* Initialise the VFS (call once at boot). */
void         vfs_init(void);

/* Mount 'root' at the given absolute path (e.g. "/").
   Returns 0 on success, -1 if the mount table is full. */
int          vfs_mount(const char* path, vfs_node_t* root);

/* ---- Node-level operations (used by backends / upper layers) ---- */
int          vfs_node_read   (vfs_node_t* node, unsigned int offset,
                              unsigned int size, unsigned char* buf);
int          vfs_node_write  (vfs_node_t* node, unsigned int offset,
                              unsigned int size, const unsigned char* buf);
int          vfs_node_readdir(vfs_node_t* node, unsigned int index,
                              char* name_out, unsigned int name_max);
vfs_node_t*  vfs_node_finddir(vfs_node_t* node, const char* name);
/* Create a new entry inside 'dir'. Returns a node on success, NULL on error. */
vfs_node_t*  vfs_node_create (vfs_node_t* dir, const char* name, unsigned int flags);
/* Remove entry 'name' from 'dir'. Returns 0 on success, -1 on error. */
int          vfs_node_unlink (vfs_node_t* dir, const char* name);

/* ---- Path resolution ---- */
/* Walk 'path' through the mount table and return the target node, or NULL. */
vfs_node_t*  vfs_resolve(const char* path);
/* Create a new file (VFS_FLAG_FILE) or directory (VFS_FLAG_DIR) at 'path'.
   Returns 0 on success, -1 on error (parent not found, backend unsupported, etc.). */
int          vfs_create  (const char* path, unsigned int flags);
/* Remove the file or directory at 'path'. Returns 0 on success, -1 on error. */
int          vfs_delete  (const char* path);
/* POSIX-style alias for delete(path). */
int          vfs_unlink  (const char* path);
/* Rename/move a path. Current generic implementation supports files.
   Returns 0 on success, -1 on error. */
int          vfs_rename  (const char* old_path, const char* new_path);
/* Retrieve basic metadata for a path (size + flags). */
int          vfs_stat    (const char* path, vfs_stat_t* out);
/* Change/get UNIX-like permission bits for an absolute path. */
int          vfs_chmod   (const char* path, unsigned int mode);
int          vfs_get_mode(const char* path, unsigned int* mode_out);

/* ---- File-descriptor API ---- */
/* Open a file/directory by absolute path. Returns fd >= 0 or -1 on error. */
int          vfs_open    (const char* path);
/* Open with access/creation flags (VFS_O_*). */
int          vfs_open_flags(const char* path, unsigned int open_flags);
/* Close a file descriptor. */
void         vfs_close   (int fd);
/* Read up to 'size' bytes from fd into buf. Advances the internal offset.
   Returns bytes read, 0 at EOF, or -1 on error. */
int          vfs_read    (int fd, unsigned char* buf, unsigned int size);
/* Write 'size' bytes from buf into fd. Advances the internal offset.
   Returns bytes written or -1 on error. */
int          vfs_write   (int fd, const unsigned char* buf, unsigned int size);
/* Seek within a file. Returns the new offset or -1 on error. */
int          vfs_seek    (int fd, int offset, int whence);
/* Read the name of the entry at 'index' inside the directory fd.
   Returns 0 on success, -1 when out of range or fd is not a directory. */
int          vfs_readdir (int fd, unsigned int index,
                          char* name_out, unsigned int name_max);

/* ---- FD introspection ---- */
int          vfs_fd_valid (int fd);
vfs_node_t*  vfs_fd_node  (int fd);
unsigned int vfs_fd_offset(int fd);

/* ---- Per-task file-descriptor table ---- */
typedef struct {
    vfs_node_t*  node;
    unsigned int offset;
   unsigned int open_flags;
    int          used;
} vfs_fd_entry_t;

/* Initialise a fresh fd table (zero all slots, install tty on fd 0/1/2
   if a TTY node has been registered via vfs_set_tty_node). */
void vfs_task_fd_init    (vfs_fd_entry_t* table);
/* Close all open FDs in a table (call on task teardown). */
void vfs_task_fd_close_all(vfs_fd_entry_t* table);
/* Copy src fd table into dst, incrementing ref_count on every shared node.
   Used by fork to give the child the same open files as the parent. */
void vfs_task_fd_inherit (vfs_fd_entry_t* dst, const vfs_fd_entry_t* src);

/* Register the global TTY node used for fd 0/1/2.
   Call once after tty_vfs_init() and before spawning user tasks. */
void vfs_set_tty_node(vfs_node_t* node);

/* Install the TTY node on fd 0, 1, 2 of a given fd table (if those
   slots are currently unused and a TTY node is registered). */
void vfs_install_stdio(vfs_fd_entry_t* table);

#endif
