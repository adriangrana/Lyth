#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "task.h"
#include "heap.h"
#include "fs.h"

unsigned int syscall_callback(unsigned int number,
                              unsigned int arg0,
                              unsigned int arg1,
                              unsigned int arg2,
                              unsigned int arg3) {
    (void)arg3;

    switch (number) {
        case SYSCALL_WRITE:
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
            return (unsigned int)kmalloc(arg0);

        case SYSCALL_FREE:
            kfree((void*)arg0);
            return 0;

        case SYSCALL_FS_COUNT:
            return (unsigned int)fs_count();

        case SYSCALL_FS_NAME_AT:
            return (unsigned int)fs_name_at((int)arg0);

        case SYSCALL_FS_READ:
            return (unsigned int)fs_read((const char*)arg0, (char*)arg1, arg2);

        case SYSCALL_FS_SIZE:
            return fs_size((const char*)arg0);

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

int syscall_fs_count(void) {
    return (int)syscall_invoke(SYSCALL_FS_COUNT, 0, 0, 0, 0);
}

const char* syscall_fs_name_at(int index) {
    return (const char*)syscall_invoke(SYSCALL_FS_NAME_AT, (unsigned int)index, 0, 0, 0);
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
