#include "pipe.h"
#include "heap.h"
#include "task.h"
#include "string.h"
#include <stdint.h>

#define PIPE_BUFFER_SIZE 4096U
#define PIPE_E_WOULD_BLOCK (-2)
#define PIPE_E_BROKEN      (-3)

typedef struct {
    unsigned char buffer[PIPE_BUFFER_SIZE];
    unsigned int  head;
    unsigned int  tail;
    unsigned int  count;
    int readers_open;
    int writers_open;
} pipe_shared_t;

typedef struct {
    pipe_shared_t* shared;
    int is_read_end;
    int nonblock;
} pipe_end_t;

static unsigned int pipe_irq_save(void) {
    unsigned int flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void pipe_irq_restore(unsigned int flags) {
    if (flags & 0x200U) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static int pipe_read_op(vfs_node_t* node, unsigned int offset,
                        unsigned int size, unsigned char* buf) {
    pipe_end_t* end;
    pipe_shared_t* sh;
    unsigned int flags;
    unsigned int i;

    (void)offset;

    if (!node || !buf || size == 0U) return -1;
    end = (pipe_end_t*)node->impl;
    if (!end || !end->shared || !end->is_read_end) return -1;

    for (;;) {
        flags = pipe_irq_save();
        sh = end->shared;

        if (sh->count > 0U) {
            unsigned int take = (size < sh->count) ? size : sh->count;
            for (i = 0; i < take; i++) {
                buf[i] = sh->buffer[sh->tail];
                sh->tail = (sh->tail + 1U) % PIPE_BUFFER_SIZE;
            }
            sh->count -= take;
            pipe_irq_restore(flags);
            return (int)take;
        }

        if (!sh->writers_open) {
            pipe_irq_restore(flags);
            return 0; /* EOF */
        }

        if (end->nonblock) {
            pipe_irq_restore(flags);
            return PIPE_E_WOULD_BLOCK;
        }

        pipe_irq_restore(flags);
        task_sleep(1);
    }
}

static int pipe_write_op(vfs_node_t* node, unsigned int offset,
                         unsigned int size, const unsigned char* buf) {
    pipe_end_t* end;
    pipe_shared_t* sh;
    unsigned int flags;
    unsigned int written = 0U;
    unsigned int i;

    (void)offset;

    if (!node || !buf || size == 0U) return 0;
    end = (pipe_end_t*)node->impl;
    if (!end || !end->shared || end->is_read_end) return -1;

    for (;;) {
        flags = pipe_irq_save();
        sh = end->shared;

        if (!sh->readers_open) {
            pipe_irq_restore(flags);
            return written > 0U ? (int)written : PIPE_E_BROKEN;
        }

        if (sh->count < PIPE_BUFFER_SIZE) {
            unsigned int space = PIPE_BUFFER_SIZE - sh->count;
            unsigned int remain = size - written;
            unsigned int put = (remain < space) ? remain : space;

            for (i = 0; i < put; i++) {
                sh->buffer[sh->head] = buf[written + i];
                sh->head = (sh->head + 1U) % PIPE_BUFFER_SIZE;
            }
            sh->count += put;
            written += put;

            pipe_irq_restore(flags);

            if (written >= size) {
                return (int)written;
            }

            if (end->nonblock) {
                return (int)written;
            }

            /* Partial write in blocking mode: return now (POSIX-compatible). */
            return (int)written;
        }

        if (end->nonblock) {
            pipe_irq_restore(flags);
            return written > 0U ? (int)written : PIPE_E_WOULD_BLOCK;
        }

        pipe_irq_restore(flags);

        if (written > 0U) {
            return (int)written;
        }

        task_sleep(1);
    }
}

static void pipe_close_op(vfs_node_t* node) {
    pipe_end_t* end;
    pipe_shared_t* sh;

    if (!node) return;
    end = (pipe_end_t*)node->impl;
    if (!end) return;

    sh = end->shared;
    if (sh) {
        if (end->is_read_end) sh->readers_open = 0;
        else                  sh->writers_open = 0;

        if (!sh->readers_open && !sh->writers_open) {
            kfree(sh);
        }
    }

    kfree(end);
    node->impl = 0;
}

static vfs_ops_t g_pipe_ops = {
    .read = pipe_read_op,
    .write = pipe_write_op,
    .readdir = 0,
    .finddir = 0,
    .open = 0,
    .close = pipe_close_op,
    .create = 0,
    .unlink = 0,
};

static vfs_node_t* pipe_make_end_node(const char* name,
                                      pipe_shared_t* shared,
                                      int is_read_end,
                                      int nonblock) {
    vfs_node_t* node;
    pipe_end_t* end;
    unsigned int i;

    node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return 0;
    end = (pipe_end_t*)kmalloc(sizeof(pipe_end_t));
    if (!end) {
        kfree(node);
        return 0;
    }

    for (i = 0U; name[i] != '\0' && i < VFS_NAME_MAX - 1U; i++) {
        node->name[i] = name[i];
    }
    node->name[i] = '\0';
    node->flags = VFS_FLAG_FILE | VFS_FLAG_DYNAMIC;
    node->size = 0;
    node->ref_count = 0;
    node->mountpoint = 0;
    node->ops = &g_pipe_ops;
    node->impl = end;

    end->shared = shared;
    end->is_read_end = is_read_end;
    end->nonblock = nonblock;

    return node;
}

int pipe_create(vfs_node_t** read_node_out, vfs_node_t** write_node_out, unsigned int flags) {
    pipe_shared_t* sh;
    vfs_node_t* rd;
    vfs_node_t* wr;
    int nonblock;

    if (!read_node_out || !write_node_out) return -1;

    sh = (pipe_shared_t*)kmalloc(sizeof(pipe_shared_t));
    if (!sh) return -1;

    sh->head = 0U;
    sh->tail = 0U;
    sh->count = 0U;
    sh->readers_open = 1;
    sh->writers_open = 1;

    nonblock = (flags & PIPE_FLAG_NONBLOCK) ? 1 : 0;

    rd = pipe_make_end_node("pipe-r", sh, 1, nonblock);
    if (!rd) {
        kfree(sh);
        return -1;
    }

    wr = pipe_make_end_node("pipe-w", sh, 0, nonblock);
    if (!wr) {
        pipe_close_op(rd);
        kfree(rd);
        return -1;
    }

    *read_node_out = rd;
    *write_node_out = wr;
    return 0;
}

int pipe_node_is_pipe(const vfs_node_t* node) {
    if (!node) return 0;
    return node->ops == &g_pipe_ops;
}

int pipe_read_ready(const vfs_node_t* node) {
    const pipe_end_t* end;
    const pipe_shared_t* sh;
    unsigned int flags;
    int ready;

    if (!pipe_node_is_pipe(node)) return 0;
    end = (const pipe_end_t*)node->impl;
    if (!end || !end->shared || !end->is_read_end) return 0;

    flags = pipe_irq_save();
    sh = end->shared;
    ready = (sh->count > 0U) || (!sh->writers_open);
    pipe_irq_restore(flags);
    return ready;
}

int pipe_write_ready(const vfs_node_t* node) {
    const pipe_end_t* end;
    const pipe_shared_t* sh;
    unsigned int flags;
    int ready;

    if (!pipe_node_is_pipe(node)) return 0;
    end = (const pipe_end_t*)node->impl;
    if (!end || !end->shared || end->is_read_end) return 0;

    flags = pipe_irq_save();
    sh = end->shared;
    ready = sh->readers_open && (sh->count < PIPE_BUFFER_SIZE);
    pipe_irq_restore(flags);
    return ready;
}

int pipe_write_has_readers(const vfs_node_t* node) {
    const pipe_end_t* end;
    const pipe_shared_t* sh;
    unsigned int flags;
    int yes;

    if (!pipe_node_is_pipe(node)) return 0;
    end = (const pipe_end_t*)node->impl;
    if (!end || !end->shared || end->is_read_end) return 0;

    flags = pipe_irq_save();
    sh = end->shared;
    yes = sh->readers_open;
    pipe_irq_restore(flags);
    return yes;
}
