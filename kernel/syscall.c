#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "task.h"
#include "heap.h"
#include "fs.h"
#include "vfs.h"
#include "paging.h"
#include "string.h"

#define SYSCALL_MAX_TEXT_LENGTH 1024U
#define SYSCALL_MAX_PATH_LENGTH 256U
#define USER_HEAP_ALIGNMENT 8U

typedef struct user_heap_block {
    unsigned int size;
    unsigned int free;
    unsigned int next;
} user_heap_block_t;

static unsigned int syscall_align_up(unsigned int size, unsigned int alignment) {
    return (size + alignment - 1U) & ~(alignment - 1U);
}

static int syscall_from_user_mode(void) {
    return task_is_running() && task_current_is_user_mode();
}

static int syscall_validate_user_string(const char* text, unsigned int max_length) {
    if (!syscall_from_user_mode()) {
        return text != 0;
    }

    return paging_user_string_is_accessible(text, max_length);
}

static int syscall_validate_user_buffer(const void* buffer, unsigned int size) {
    if (!syscall_from_user_mode()) {
        return buffer != 0;
    }

    return paging_user_buffer_is_accessible(buffer, size);
}

static int syscall_copy_string_to_buffer(const char* source, char* destination, unsigned int buffer_size) {
    unsigned int length;

    if (source == 0 || destination == 0 || buffer_size == 0) {
        return -1;
    }

    length = str_length(source);
    if (length >= buffer_size) {
        length = buffer_size - 1;
    }

    for (unsigned int i = 0; i < length; i++) {
        destination[i] = source[i];
    }

    destination[length] = '\0';
    return (int)length;
}

static user_heap_block_t* user_heap_head(void) {
    uint32_t heap_base = task_current_user_heap_base();
    uint32_t heap_size = task_current_user_heap_size();
    user_heap_block_t* head;

    if (heap_base == 0 || heap_size <= sizeof(user_heap_block_t)) {
        return 0;
    }

    head = (user_heap_block_t*)(uintptr_t)heap_base;
    if (head->size == 0 && head->free == 0 && head->next == 0) {
        head->size = heap_size - sizeof(user_heap_block_t);
        head->free = 1;
        head->next = 0;
    }

    return head;
}

static void user_heap_split_block(user_heap_block_t* block, unsigned int size) {
    user_heap_block_t* next_block;
    unsigned int remaining;

    if (block == 0 || block->size <= size + sizeof(user_heap_block_t) + USER_HEAP_ALIGNMENT) {
        return;
    }

    remaining = block->size - size - sizeof(user_heap_block_t);
    next_block = (user_heap_block_t*)((unsigned char*)(block + 1) + size);
    next_block->size = remaining;
    next_block->free = 1;
    next_block->next = block->next;

    block->size = size;
    block->next = (unsigned int)(uintptr_t)next_block;
}

static void user_heap_coalesce(void) {
    user_heap_block_t* block = user_heap_head();

    while (block != 0 && block->next != 0) {
        user_heap_block_t* next_block = (user_heap_block_t*)(uintptr_t)block->next;

        if (block->free && next_block->free) {
            block->size += sizeof(user_heap_block_t) + next_block->size;
            block->next = next_block->next;
            continue;
        }

        block = next_block;
    }
}

static void* user_heap_alloc(unsigned int size) {
    user_heap_block_t* block;

    if (size == 0) {
        return 0;
    }

    block = user_heap_head();
    if (block == 0) {
        return 0;
    }

    size = syscall_align_up(size, USER_HEAP_ALIGNMENT);
    while (block != 0) {
        if (block->free && block->size >= size) {
            user_heap_split_block(block, size);
            block->free = 0;
            return (void*)(block + 1);
        }

        block = block->next != 0 ? (user_heap_block_t*)(uintptr_t)block->next : 0;
    }

    return 0;
}

static void user_heap_free(void* ptr) {
    user_heap_block_t* block;

    if (ptr == 0 || !paging_user_buffer_is_accessible(ptr, 1)) {
        return;
    }

    block = ((user_heap_block_t*)ptr) - 1;
    if (!paging_user_buffer_is_accessible(block, sizeof(user_heap_block_t))) {
        return;
    }

    block->free = 1;
    user_heap_coalesce();
}

unsigned int syscall_callback(unsigned int number,
                              unsigned int arg0,
                              unsigned int arg1,
                              unsigned int arg2,
                              unsigned int arg3) {
    (void)arg3;

    switch (number) {
        case SYSCALL_WRITE:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_TEXT_LENGTH)) {
                return (unsigned int)-1;
            }
            terminal_print((const char*)arg0);
            return 0;

        case SYSCALL_GET_TICKS:
            return timer_get_ticks();

        case SYSCALL_SLEEP:
            task_sleep(arg0);
            return 0;

        case SYSCALL_YIELD:
            task_yield();
            return 0;

        case SYSCALL_ALLOC:
            if (syscall_from_user_mode()) {
                return (unsigned int)user_heap_alloc(arg0);
            }
            return (unsigned int)kmalloc(arg0);

        case SYSCALL_FREE:
            if (syscall_from_user_mode()) {
                user_heap_free((void*)arg0);
                return 0;
            }
            kfree((void*)arg0);
            return 0;

        case SYSCALL_EXIT:
            task_exit((int)arg0);
            return 0;

        case SYSCALL_FS_COUNT:
            return (unsigned int)fs_count();

        case SYSCALL_FS_NAME_AT:
            if (syscall_from_user_mode()) {
                return 0;
            }
            return (unsigned int)fs_name_at((int)arg0);

        case SYSCALL_FS_NAME_COPY: {
            const char* name = fs_name_at((int)arg0);

            if (!syscall_validate_user_buffer((const void*)arg1, arg2)) {
                return (unsigned int)-1;
            }

            return (unsigned int)syscall_copy_string_to_buffer(name, (char*)arg1, arg2);
        }

        case SYSCALL_FS_READ:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH) ||
                !syscall_validate_user_buffer((const void*)arg1, arg2)) {
                return (unsigned int)-1;
            }
            return (unsigned int)fs_read((const char*)arg0, (char*)arg1, arg2);

        case SYSCALL_FS_SIZE:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                return 0;
            }
            return fs_size((const char*)arg0);

        /* ---- VFS / file-descriptor syscalls ---- */

        case SYSCALL_VFS_OPEN:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH))
                return (unsigned int)-1;
            return (unsigned int)vfs_open((const char*)arg0);

        case SYSCALL_VFS_CLOSE:
            vfs_close((int)arg0);
            return 0;

        case SYSCALL_VFS_READ:
            if (!syscall_validate_user_buffer((const void*)arg1, arg2))
                return (unsigned int)-1;
            return (unsigned int)vfs_read((int)arg0, (unsigned char*)arg1, arg2);

        case SYSCALL_VFS_WRITE:
            if (!syscall_validate_user_buffer((const void*)arg1, arg2))
                return (unsigned int)-1;
            return (unsigned int)vfs_write((int)arg0, (const unsigned char*)arg1, arg2);

        case SYSCALL_VFS_SEEK:
            return (unsigned int)vfs_seek((int)arg0, (int)arg1, (int)arg2);

        case SYSCALL_VFS_READDIR:
            if (!syscall_validate_user_buffer((const void*)arg2, arg3))
                return (unsigned int)-1;
            return (unsigned int)vfs_readdir((int)arg0, arg1,
                                             (char*)arg2, arg3);

        default:
            return 0;
    }
}

unsigned int syscall_invoke(unsigned int number,
                            unsigned int arg0,
                            unsigned int arg1,
                            unsigned int arg2,
                            unsigned int arg3) {
    unsigned int result;

    __asm__ volatile (
        "int $0x80"
        : "=a"(result)
        : "a"(number), "b"(arg0), "c"(arg1), "d"(arg2), "S"(arg3)
        : "memory"
    );

    return result;
}

void syscall_write(const char* text) {
    syscall_invoke(SYSCALL_WRITE, (unsigned int)text, 0, 0, 0);
}

unsigned int syscall_get_ticks(void) {
    return syscall_invoke(SYSCALL_GET_TICKS, 0, 0, 0, 0);
}

void syscall_sleep(unsigned int ticks) {
    syscall_invoke(SYSCALL_SLEEP, ticks, 0, 0, 0);
}

void syscall_yield(void) {
    syscall_invoke(SYSCALL_YIELD, 0, 0, 0, 0);
}

void* syscall_alloc(unsigned int size) {
    return (void*)syscall_invoke(SYSCALL_ALLOC, size, 0, 0, 0);
}

void syscall_free(void* ptr) {
    syscall_invoke(SYSCALL_FREE, (unsigned int)ptr, 0, 0, 0);
}

void syscall_exit(int exit_code) {
    syscall_invoke(SYSCALL_EXIT, (unsigned int)exit_code, 0, 0, 0);
}

int syscall_fs_count(void) {
    return (int)syscall_invoke(SYSCALL_FS_COUNT, 0, 0, 0, 0);
}

int syscall_fs_name(int index, char* buffer, unsigned int buffer_size) {
    return (int)syscall_invoke(SYSCALL_FS_NAME_COPY,
                               (unsigned int)index,
                               (unsigned int)buffer,
                               buffer_size,
                               0);
}

int syscall_fs_read(const char* name, char* buffer, unsigned int buffer_size) {
    return (int)syscall_invoke(SYSCALL_FS_READ,
                               (unsigned int)name,
                               (unsigned int)buffer,
                               buffer_size,
                               0);
}

unsigned int syscall_fs_size(const char* name) {
    return syscall_invoke(SYSCALL_FS_SIZE, (unsigned int)name, 0, 0, 0);
}

int syscall_vfs_open(const char* path) {
    return (int)syscall_invoke(SYSCALL_VFS_OPEN, (unsigned int)path, 0, 0, 0);
}

void syscall_vfs_close(int fd) {
    syscall_invoke(SYSCALL_VFS_CLOSE, (unsigned int)fd, 0, 0, 0);
}

int syscall_vfs_read(int fd, unsigned char* buf, unsigned int size) {
    return (int)syscall_invoke(SYSCALL_VFS_READ,
                               (unsigned int)fd,
                               (unsigned int)buf,
                               size, 0);
}

int syscall_vfs_write(int fd, const unsigned char* buf, unsigned int size) {
    return (int)syscall_invoke(SYSCALL_VFS_WRITE,
                               (unsigned int)fd,
                               (unsigned int)buf,
                               size, 0);
}

int syscall_vfs_seek(int fd, int offset, int whence) {
    return (int)syscall_invoke(SYSCALL_VFS_SEEK,
                               (unsigned int)fd,
                               (unsigned int)offset,
                               (unsigned int)whence, 0);
}

int syscall_vfs_readdir(int fd, unsigned int index,
                        char* name_out, unsigned int name_max) {
    return (int)syscall_invoke(SYSCALL_VFS_READDIR,
                               (unsigned int)fd,
                               index,
                               (unsigned int)name_out,
                               name_max);
}
