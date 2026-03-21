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
    SYSCALL_FS_NAME_COPY = 12
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

#endif
