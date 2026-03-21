#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "rtc.h"
#include "task.h"
#include "usermode.h"
#include "heap.h"
#include "fs.h"
#include "vfs.h"
#include "paging.h"
#include "string.h"
#include <stdint.h>

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

static int syscall_deny_user_legacy_fs(void) {
    if (syscall_from_user_mode()) {
        task_set_errno(1); /* EPERM */
        return 1;
    }
    return 0;
}

static unsigned int syscall_vfs_flag_mask(void) {
    return VFS_O_RDONLY | VFS_O_WRONLY | VFS_O_APPEND | VFS_O_CREAT |
           VFS_O_TRUNC | VFS_O_EXCL | VFS_O_DIRECTORY;
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

unsigned int syscall_exec_interrupt(unsigned int frame_esp,
                                    unsigned int path,
                                    unsigned int foreground) {
    (void)foreground;

    if (!task_current_is_user_mode()) {
        task_set_errno(1); /* EPERM */
        return (unsigned int)-1;
    }

    if (!syscall_validate_user_string((const char*)path, SYSCALL_MAX_PATH_LENGTH)) {
        task_set_errno(22); /* EINVAL */
        return (unsigned int)-1;
    }

    if (usermode_exec_current_vfs_argv((const char*)path,
                                       0, 0, 0, 0,
                                       frame_esp) < 0) {
        task_set_errno(2); /* ENOENT / generic */
        return (unsigned int)-1;
    }

    return 0;
}

unsigned int syscall_execv_interrupt(unsigned int frame_esp,
                                     unsigned int path,
                                     unsigned int foreground,
                                     unsigned int argv,
                                     unsigned int argc) {
    const char* kpath = (const char*)path;
    const char** uargv = (const char**)argv;
    int uargc = (int)argc;
    const char* kargv[16];
    int valid_argc = 0;

    (void)foreground;

    if (!task_current_is_user_mode()) {
        task_set_errno(1); /* EPERM */
        return (unsigned int)-1;
    }

    if (!syscall_validate_user_string(kpath, SYSCALL_MAX_PATH_LENGTH)) {
        task_set_errno(22);
        return (unsigned int)-1;
    }

    if (uargv != 0 && uargc > 0) {
        int lim = uargc < 16 ? uargc : 16;
        if (!syscall_validate_user_buffer(uargv,
                (unsigned int)((unsigned int)lim * sizeof(char*)))) {
            task_set_errno(22);
            return (unsigned int)-1;
        }
        for (int i = 0; i < lim; i++) {
            if (!syscall_validate_user_string(uargv[i], SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22);
                return (unsigned int)-1;
            }
            kargv[valid_argc++] = uargv[i];
        }
    }

    if (usermode_exec_current_vfs_argv(kpath,
                                       valid_argc, (const char* const*)kargv,
                                       0, 0,
                                       frame_esp) < 0) {
        task_set_errno(2);
        return (unsigned int)-1;
    }

    return 0;
}

/* execve: path + NULL-terminated argv[] + NULL-terminated envp[] */
unsigned int syscall_execve_interrupt(unsigned int frame_esp,
                                      unsigned int path,
                                      unsigned int argv_ptr,
                                      unsigned int envp_ptr) {
    const char* kpath = (const char*)path;
    const char** uargv = (const char**)argv_ptr;
    const char** uenvp = (const char**)envp_ptr;
    const char* kargv[16];
    const char* kenvp[16];
    int argc = 0, envc = 0;

    if (!task_current_is_user_mode()) {
        task_set_errno(1); return (unsigned int)-1;
    }

    if (!syscall_validate_user_string(kpath, SYSCALL_MAX_PATH_LENGTH)) {
        task_set_errno(22); return (unsigned int)-1;
    }

    /* Copy NULL-terminated argv */
    if (uargv != 0) {
        for (int i = 0; i < 16; i++) {
            if (!syscall_validate_user_buffer(&uargv[i], sizeof(char*)))
                break;
            if (uargv[i] == 0) break;
            if (!syscall_validate_user_string(uargv[i], SYSCALL_MAX_PATH_LENGTH))
                break;
            kargv[argc++] = uargv[i];
        }
    }

    /* Copy NULL-terminated envp */
    if (uenvp != 0) {
        for (int i = 0; i < 16; i++) {
            if (!syscall_validate_user_buffer(&uenvp[i], sizeof(char*)))
                break;
            if (uenvp[i] == 0) break;
            if (!syscall_validate_user_string(uenvp[i], SYSCALL_MAX_PATH_LENGTH))
                break;
            kenvp[envc++] = uenvp[i];
        }
    }

    if (usermode_exec_current_vfs_argv(kpath,
                                       argc, (const char* const*)kargv,
                                       envc, (const char* const*)kenvp,
                                       frame_esp) < 0) {
        task_set_errno(2);
        return (unsigned int)-1;
    }

    return 0;
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
            if (syscall_deny_user_legacy_fs()) return (unsigned int)-1;
            return (unsigned int)fs_count();

        case SYSCALL_FS_NAME_AT:
            if (syscall_deny_user_legacy_fs()) return (unsigned int)-1;
            return (unsigned int)fs_name_at((int)arg0);

        case SYSCALL_FS_NAME_COPY: {
            const char* name = fs_name_at((int)arg0);

            if (syscall_deny_user_legacy_fs()) return (unsigned int)-1;

            if (!syscall_validate_user_buffer((const void*)arg1, arg2)) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }

            return (unsigned int)syscall_copy_string_to_buffer(name, (char*)arg1, arg2);
        }

        case SYSCALL_FS_READ:
            if (syscall_deny_user_legacy_fs()) return (unsigned int)-1;
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH) ||
                !syscall_validate_user_buffer((const void*)arg1, arg2)) {
                task_set_errno(22); /* EINVAL/EFAULT */
                return (unsigned int)-1;
            }
            return (unsigned int)fs_read((const char*)arg0, (char*)arg1, arg2);

        case SYSCALL_FS_SIZE:
            if (syscall_deny_user_legacy_fs()) return (unsigned int)-1;
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22); /* EINVAL */
                return 0;
            }
            return fs_size((const char*)arg0);

        /* ---- VFS / file-descriptor syscalls ---- */

        case SYSCALL_VFS_OPEN:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            if ((arg1 & ~syscall_vfs_flag_mask()) != 0U) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            {
                int fd = vfs_open_flags((const char*)arg0, arg1);
                if (fd < 0) task_set_errno(13); /* EACCES/ENOENT generic */
                return (unsigned int)fd;
            }

        case SYSCALL_VFS_CLOSE:
            vfs_close((int)arg0);
            return 0;

        case SYSCALL_VFS_READ:
            if (!syscall_validate_user_buffer((const void*)arg1, arg2)) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }
            {
                int n = vfs_read((int)arg0, (unsigned char*)arg1, arg2);
                if (n < 0) task_set_errno(9); /* EBADF/EACCES generic */
                return (unsigned int)n;
            }

        case SYSCALL_VFS_WRITE:
            if (!syscall_validate_user_buffer((const void*)arg1, arg2)) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }
            {
                int n = vfs_write((int)arg0, (const unsigned char*)arg1, arg2);
                if (n < 0) task_set_errno(9); /* EBADF/EACCES generic */
                return (unsigned int)n;
            }

        case SYSCALL_VFS_SEEK:
        {
            int off = vfs_seek((int)arg0, (int)arg1, (int)arg2);
            if (off < 0) task_set_errno(22); /* EINVAL/EBADF generic */
            return (unsigned int)off;
        }

        case SYSCALL_VFS_READDIR:
            if (arg3 == 0U || arg3 > VFS_NAME_MAX) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            if (!syscall_validate_user_buffer((const void*)arg2, arg3)) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }
            {
                int rc = vfs_readdir((int)arg0, arg1, (char*)arg2, arg3);
                if (rc < 0) task_set_errno(9); /* EBADF/ENOENT generic */
                return (unsigned int)rc;
            }

        case SYSCALL_VFS_CREATE:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            if ((arg1 & ~(VFS_FLAG_FILE | VFS_FLAG_DIR)) != 0U ||
                ((arg1 & (VFS_FLAG_FILE | VFS_FLAG_DIR)) == 0U) ||
                ((arg1 & VFS_FLAG_FILE) && (arg1 & VFS_FLAG_DIR))) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            {
                int rc = vfs_create((const char*)arg0, arg1);
                if (rc < 0) task_set_errno(13); /* EACCES/ENOSPC generic */
                return (unsigned int)rc;
            }

        case SYSCALL_VFS_DELETE:
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            {
                int rc = vfs_delete((const char*)arg0);
                if (rc < 0) task_set_errno(13); /* EACCES/ENOENT generic */
                return (unsigned int)rc;
            }

        /* ---- process management ---- */
        case SYSCALL_GETPID:
            return (unsigned int)task_current_id();

        case SYSCALL_KILL: {
            int ok = task_send_signal((int)arg0, LYTH_SIGTERM);
            if (!ok) task_set_errno(3); /* ESRCH */
            return ok ? 0U : (unsigned int)-1;
        }

        case SYSCALL_WAIT:
            task_wait_id((int)arg0);
            return 0;

        case SYSCALL_WAITPID: {
            /* arg0 = pid, arg1 = user int* status_out */
            uint32_t status_uptr = arg1;
            int ret;

            if (status_uptr != 0 &&
                !syscall_validate_user_buffer((const void*)status_uptr,
                                              sizeof(int))) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }

            ret = task_waitpid((int)arg0, status_uptr);
            return (unsigned int)ret;
        }

        case SYSCALL_EXEC: {
            int spawn_id;
            if (!syscall_validate_user_string((const char*)arg0, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }
            spawn_id = usermode_spawn_elf_vfs((const char*)arg0, (int)arg1);
            if (spawn_id < 0) task_set_errno(2); /* ENOENT / generic */
            return (unsigned int)spawn_id;
        }

        case SYSCALL_GET_ERRNO:
            return (unsigned int)task_get_errno();

        case SYSCALL_EXECV: {
            /* arg0=path, arg1=foreground, arg2=argv (user char**), arg3=argc */
            const char* path = (const char*)arg0;
            int         fg   = (int)arg1;
            const char** uargv = (const char**)arg2;
            int          uargc = (int)arg3;
            int spawn_id;
            const char* kargv[16];
            int valid_argc = 0;

            if (!syscall_validate_user_string(path, SYSCALL_MAX_PATH_LENGTH)) {
                task_set_errno(22);
                return (unsigned int)-1;
            }
            if (uargv != 0 && uargc > 0) {
                int lim = uargc < 16 ? uargc : 16;
                if (!syscall_validate_user_buffer(uargv,
                        (unsigned int)((unsigned int)lim * sizeof(char*)))) {
                    task_set_errno(22);
                    return (unsigned int)-1;
                }
                for (int i = 0; i < lim; i++) {
                    if (!syscall_validate_user_string(uargv[i],
                                                      SYSCALL_MAX_PATH_LENGTH)) {
                        task_set_errno(22);
                        return (unsigned int)-1;
                    }
                    kargv[valid_argc++] = uargv[i];
                }
            }
            spawn_id = usermode_spawn_elf_vfs_argv(path,
                            valid_argc, (const char* const*)kargv,
                            0, 0, fg);
            if (spawn_id < 0) task_set_errno(2);
            return (unsigned int)spawn_id;
        }

        case SYSCALL_SIGNAL: {
            int signum = (int)arg0;
            unsigned int handler = arg1;
            unsigned int old_handler = 0;
            int ok;

            if (!task_current_is_user_mode()) {
                task_set_errno(1); /* EPERM */
                return (unsigned int)-1;
            }

            if (handler > LYTH_SIG_IGN &&
                !syscall_validate_user_buffer((const void*)handler, 1)) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }

            ok = task_set_signal_handler(signum, handler, &old_handler);
            if (!ok) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }

            return old_handler;
        }

        case SYSCALL_KILLSIG: {
            int ok = task_send_signal((int)arg0, (int)arg1);
            if (!ok) task_set_errno(22); /* EINVAL/ESRCH */
            return ok ? 0U : (unsigned int)-1;
        }

        case SYSCALL_SIGPENDING:
            return task_pending_signals();

        case SYSCALL_SIGPROCMASK: {
            unsigned int how = arg0;
            unsigned int mask = arg1;
            unsigned int old_mask = 0;
            unsigned int* old_mask_user = (unsigned int*)arg2;
            int ok;

            if (old_mask_user != 0 &&
                !syscall_validate_user_buffer(old_mask_user, sizeof(unsigned int))) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }

            ok = task_sigprocmask(how, mask, &old_mask);
            if (!ok) {
                task_set_errno(22); /* EINVAL */
                return (unsigned int)-1;
            }

            if (old_mask_user != 0) {
                *old_mask_user = old_mask;
            }

            return old_mask;
        }

        case SYSCALL_GET_TIME: {
            /* arg0 = pointer to rtc_time_t in caller's address space */
            if (!syscall_validate_user_buffer((void*)arg0, sizeof(rtc_time_t))) {
                task_set_errno(14); /* EFAULT */
                return (unsigned int)-1;
            }
            rtc_read((rtc_time_t*)arg0);
            return 0;
        }

        case SYSCALL_GET_MONOTONIC_MS:
            return timer_get_uptime_ms();

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
    return (int)syscall_invoke(SYSCALL_VFS_OPEN,
                               (unsigned int)path,
                               VFS_O_RDWR,
                               0, 0);
}

int syscall_vfs_open_flags(const char* path, unsigned int open_flags) {
    return (int)syscall_invoke(SYSCALL_VFS_OPEN,
                               (unsigned int)path,
                               open_flags,
                               0, 0);
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

int syscall_vfs_create(const char* path, unsigned int flags) {
    return (int)syscall_invoke(SYSCALL_VFS_CREATE,
                               (unsigned int)path,
                               flags, 0, 0);
}

int syscall_vfs_delete(const char* path) {
    return (int)syscall_invoke(SYSCALL_VFS_DELETE,
                               (unsigned int)path,
                               0, 0, 0);
}

int syscall_getpid(void) {
    return (int)syscall_invoke(SYSCALL_GETPID, 0, 0, 0, 0);
}

int syscall_kill(int id) {
    return (int)syscall_invoke(SYSCALL_KILL, (unsigned int)id, 0, 0, 0);
}

int syscall_killsig(int id, int signum) {
    return (int)syscall_invoke(SYSCALL_KILLSIG,
                               (unsigned int)id,
                               (unsigned int)signum,
                               0, 0);
}

void syscall_wait(int id) {
    syscall_invoke(SYSCALL_WAIT, (unsigned int)id, 0, 0, 0);
}

int syscall_waitpid(int pid, int* status) {
    return (int)syscall_invoke(SYSCALL_WAITPID,
                               (unsigned int)pid,
                               (unsigned int)(uintptr_t)status,
                               0, 0);
}

int syscall_exec(const char* path, int foreground) {
    return (int)syscall_invoke(SYSCALL_EXEC,
                               (unsigned int)path,
                               (unsigned int)foreground,
                               0, 0);
}

int syscall_get_errno(void) {
    return (int)syscall_invoke(SYSCALL_GET_ERRNO, 0, 0, 0, 0);
}

int syscall_fork(void) {
    return (int)syscall_invoke(SYSCALL_FORK, 0, 0, 0, 0);
}

int syscall_execv(const char* path, int foreground,
                  const char* const* argv, int argc) {
    return (int)syscall_invoke(SYSCALL_EXECV,
                               (unsigned int)path,
                               (unsigned int)foreground,
                               (unsigned int)argv,
                               (unsigned int)argc);
}

int syscall_execve(const char* path,
                   const char* const* argv,
                   const char* const* envp) {
    return (int)syscall_invoke(SYSCALL_EXECVE,
                               (unsigned int)path,
                               (unsigned int)argv,
                               (unsigned int)envp,
                               0);
}

sys_signal_handler_t syscall_signal(int signum, sys_signal_handler_t handler) {
    return (sys_signal_handler_t)(uintptr_t)
        syscall_invoke(SYSCALL_SIGNAL,
                       (unsigned int)signum,
                       (unsigned int)(uintptr_t)handler,
                       0, 0);
}

unsigned int syscall_sigpending(void) {
    return syscall_invoke(SYSCALL_SIGPENDING, 0, 0, 0, 0);
}

int syscall_sigprocmask(unsigned int how, unsigned int mask, unsigned int* old_mask_out) {
    return (int)syscall_invoke(SYSCALL_SIGPROCMASK,
                               how,
                               mask,
                               (unsigned int)(uintptr_t)old_mask_out,
                               0);
}

int syscall_get_time(void* rtc_time_buf) {
    return (int)syscall_invoke(SYSCALL_GET_TIME,
                               (unsigned int)(uintptr_t)rtc_time_buf,
                               0, 0, 0);
}

unsigned int syscall_get_monotonic_ms(void) {
    return syscall_invoke(SYSCALL_GET_MONOTONIC_MS, 0, 0, 0, 0);
}
