#ifndef SYSCALL_H
#define SYSCALL_H

enum {
    SYSCALL_WRITE = 1,
    SYSCALL_GET_TICKS = 2,
    SYSCALL_SLEEP = 3,
    SYSCALL_YIELD = 4,
    SYSCALL_ALLOC = 5,
    SYSCALL_FREE = 6,
    SYSCALL_FS_COUNT = 7,
    SYSCALL_FS_NAME_AT = 8,
    SYSCALL_FS_READ = 9,
    SYSCALL_FS_SIZE = 10,
    SYSCALL_EXIT = 11,
    SYSCALL_FS_NAME_COPY = 12,
    /* VFS file-descriptor syscalls */
    SYSCALL_VFS_OPEN     = 13,  /* open(path)              -> fd   */
    SYSCALL_VFS_CLOSE    = 14,  /* close(fd)               -> 0    */
    SYSCALL_VFS_READ     = 15,  /* read(fd, buf, size)     -> n    */
    SYSCALL_VFS_WRITE    = 16,  /* write(fd, buf, size)    -> n    */
    SYSCALL_VFS_SEEK     = 17,  /* seek(fd, off, whence)   -> off  */
    SYSCALL_VFS_READDIR  = 18   /* readdir(fd,idx,buf,max) -> 0/-1 */
};

unsigned int syscall_callback(unsigned int number,
                              unsigned int arg0,
                              unsigned int arg1,
                              unsigned int arg2,
                              unsigned int arg3);
unsigned int syscall_invoke(unsigned int number,
                            unsigned int arg0,
                            unsigned int arg1,
                            unsigned int arg2,
                            unsigned int arg3);

void syscall_write(const char* text);
unsigned int syscall_get_ticks(void);
void syscall_sleep(unsigned int ticks);
void syscall_yield(void);
void* syscall_alloc(unsigned int size);
void syscall_free(void* ptr);
void syscall_exit(int exit_code);
int syscall_fs_count(void);
int syscall_fs_name(int index, char* buffer, unsigned int buffer_size);
int syscall_fs_read(const char* name, char* buffer, unsigned int buffer_size);
unsigned int syscall_fs_size(const char* name);

/* VFS / file-descriptor helpers */
int  syscall_vfs_open   (const char* path);
void syscall_vfs_close  (int fd);
int  syscall_vfs_read   (int fd, unsigned char* buf, unsigned int size);
int  syscall_vfs_write  (int fd, const unsigned char* buf, unsigned int size);
int  syscall_vfs_seek   (int fd, int offset, int whence);
int  syscall_vfs_readdir(int fd, unsigned int index,
                         char* name_out, unsigned int name_max);

#endif
