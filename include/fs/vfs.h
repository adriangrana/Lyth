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
#define VFS_MAX_MOUNTS  8

/* ---- Seek whence constants ---- */
#define VFS_SEEK_SET 0
#define VFS_SEEK_CUR 1
#define VFS_SEEK_END 2

typedef struct vfs_node vfs_node_t;

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
} vfs_ops_t;

/* ---- VFS node ---- */
struct vfs_node {
    char         name[VFS_NAME_MAX]; /* entry name (no leading path) */
    unsigned int flags;              /* VFS_FLAG_* */
    unsigned int size;               /* file size in bytes (0 for dirs) */
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

/* ---- Path resolution ---- */
/* Walk 'path' through the mount table and return the target node, or NULL. */
vfs_node_t*  vfs_resolve(const char* path);

/* ---- File-descriptor API ---- */
/* Open a file/directory by absolute path. Returns fd >= 0 or -1 on error. */
int          vfs_open    (const char* path);
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

#endif
