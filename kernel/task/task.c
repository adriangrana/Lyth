#include "task.h"
#include "heap.h"
#include "timer.h"
#include "syscall.h"
#include "gdt.h"
#include "paging.h"
#include "physmem.h"
#include "shm.h"
#include "mqueue.h"
#include "assert.h"
#include "rlimit.h"
#include "terminal.h"
#include "ugdb.h"
#include <stdint.h>

#define TASK_MAX_COUNT 16
#define TASK_NAME_MAX 24
#define TASK_STACK_SIZE 16384
#define TASK_USER_STACK_SIZE 4096

typedef struct {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp_placeholder;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
} task_stack_frame_t;

typedef struct {
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp_placeholder;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int user_ss;
} task_user_stack_frame_t;

typedef struct {
    int used;
    int id;
    char name[TASK_NAME_MAX];
    task_state_t state;
    int foreground;
    int cancel_requested;
    unsigned int wake_tick;
    int blocked_event_id;
    task_priority_t priority;
    task_step_fn step;
    void* data;
    unsigned int data_size;
    int exit_code;
    unsigned int saved_esp;
    unsigned char* stack;
    unsigned char* user_stack;
    unsigned int user_entry;
    int user_mode;
    uint32_t user_physical_base;
    uint32_t user_heap_base;
    uint32_t user_heap_size;
    uint32_t user_initial_esp;               /* custom initial user ESP (0 = top of stack) */
    uint32_t* page_directory;
    vfs_fd_entry_t fd_table[VFS_MAX_FD]; /* per-task open file descriptors */
    unsigned int fd_limit_soft;          /* soft FD limit (RLIMIT_NOFILE cur) */
    unsigned int fd_limit_hard;          /* hard FD limit (RLIMIT_NOFILE max) */
    int k_errno;                         /* per-task errno for syscalls */
    int parent_id;                       /* PID of the task that spawned this one */
    unsigned int pending_signals;        /* bitmask of pending signals */
    unsigned int signal_mask;            /* blocked signals bitmask */
    unsigned int signal_handlers[LYTH_SIGNAL_MAX + 1]; /* 0/1 special or user handler */
    uint32_t signal_trampoline_va;       /* user-space stub to restore stack after handler */
    uint32_t waitpid_status_uptr;        /* user vaddr where WAITPID status is written on wake */
    int sched_next;                      /* next index in ready queue, -1 = tail / not queued */
    unsigned int uid;                    /* real user id */
    unsigned int gid;                    /* real group id */
    unsigned int euid;                   /* effective user id */
    unsigned int egid;                   /* effective group id */
    unsigned int supp_groups[TASK_MAX_SUPP_GROUPS]; /* supplementary groups */
    int supp_group_count;
    shm_mapping_t shm_mappings[SHM_MAX_MAPPINGS];
    /* alarm()/setitimer() state */
    unsigned int alarm_tick;             /* tick at which SIGALRM fires (0 = not armed) */
    unsigned int itimer_interval_ticks; /* ITIMER_REAL reload interval (0 = one-shot) */
    int output_vc;                      /* VC index for terminal output (-1 = follow active) */
} task_entry_t;

/* ── Per-priority ready queues ────────────────────────────────────────────
 * Intrusive singly-linked FIFO: tasks[i].sched_next is the index of the
 * next task in the same priority queue, or -1 if it is the tail.
 * sched_ready_head[p] / sched_ready_tail[p]: head and tail indices, -1=empty.
 * ──────────────────────────────────────────────────────────────────────── */
#define SCHED_PRIO_COUNT 4   /* HIGH, NORMAL, LOW, IDLE */

static task_entry_t tasks[TASK_MAX_COUNT];
static int current_task_index = -1;
static int next_task_id = 1;
static int foreground_task_id = -1;
static int sched_ready_head[SCHED_PRIO_COUNT]; /* heads of per-priority FIFO queues */
static int sched_ready_tail[SCHED_PRIO_COUNT]; /* tails of per-priority FIFO queues */
static int    idle_task_id    = -1;
static unsigned int idle_tick_count  = 0;
static unsigned int ctx_switch_count = 0;
static unsigned int idle_context_esp = 0;
static unsigned short kernel_code_selector = 0x08;
static unsigned short user_code_selector = GDT_USER_CODE_SELECTOR;
static unsigned short user_data_selector = GDT_USER_DATA_SELECTOR;
static void (*foreground_complete_handler)(int id, const char* name, int cancelled) = 0;
static int sys_init_pid = -1;   /* PID of the init/reaper task */

static void task_thread_bootstrap(void);
static void complete_task(task_entry_t* task);

static void task_reset_shm_mappings(task_entry_t* task) {
    if (task == 0) {
        return;
    }

    for (int i = 0; i < SHM_MAX_MAPPINGS; i++) {
        task->shm_mappings[i].used = 0;
        task->shm_mappings[i].segment_id = 0;
        task->shm_mappings[i].base = 0U;
        task->shm_mappings[i].size = 0U;
    }
}

/* ── Queue helper: derive slot index from pointer ─────────────────────── */
static int task_slot_index(const task_entry_t* t) {
    int idx = (int)(t - tasks);
    return (idx >= 0 && idx < TASK_MAX_COUNT) ? idx : -1;
}

static int sched_is_queued(int idx, int prio) {
    int cur;
    if (idx < 0 || idx >= TASK_MAX_COUNT || prio < 0 || prio >= SCHED_PRIO_COUNT) {
        return 0;
    }
    cur = sched_ready_head[prio];
    while (cur >= 0) {
        if (cur == idx) {
            return 1;
        }
        cur = tasks[cur].sched_next;
    }
    return 0;
}

/* Append tasks[idx] to the tail of its priority ready queue (FIFO). */
static void sched_enqueue_ready(int idx) {
    int prio;
    if (idx < 0 || idx >= TASK_MAX_COUNT || !tasks[idx].used) return;
    KASSERT(tasks[idx].state == TASK_STATE_READY);
    prio = (int)tasks[idx].priority;
    if (prio < 0 || prio >= SCHED_PRIO_COUNT) return;
    if (sched_is_queued(idx, prio)) {
        return;
    }
    tasks[idx].sched_next = -1;
    if (sched_ready_tail[prio] < 0) {
        sched_ready_head[prio] = idx;
        sched_ready_tail[prio] = idx;
    } else {
        tasks[sched_ready_tail[prio]].sched_next = idx;
        sched_ready_tail[prio] = idx;
    }
}

/* Remove and return the head of priority queue prio (-1 if empty). */
static int sched_dequeue_ready(int prio) {
    int idx;
    if (prio < 0 || prio >= SCHED_PRIO_COUNT) return -1;
    idx = sched_ready_head[prio];
    if (idx < 0) return -1;
    sched_ready_head[prio] = tasks[idx].sched_next;
    if (sched_ready_head[prio] < 0) sched_ready_tail[prio] = -1;
    tasks[idx].sched_next = -1;
    return idx;
}

/* Remove tasks[idx] from its priority ready queue (no-op if not present). */
static void sched_remove_from_ready(int idx) {
    int prio, cur, prev;
    if (idx < 0 || idx >= TASK_MAX_COUNT || !tasks[idx].used) return;
    prio = (int)tasks[idx].priority;
    if (prio < 0 || prio >= SCHED_PRIO_COUNT) return;
    prev = -1;
    cur  = sched_ready_head[prio];
    while (cur >= 0) {
        if (cur == idx) {
            if (prev < 0) sched_ready_head[prio] = tasks[cur].sched_next;
            else          tasks[prev].sched_next  = tasks[cur].sched_next;
            if (sched_ready_tail[prio] == idx) sched_ready_tail[prio] = prev;
            tasks[idx].sched_next = -1;
            return;
        }
        prev = cur;
        cur  = tasks[cur].sched_next;
    }
    tasks[idx].sched_next = -1;
}

/* Idle task: runs HLT while no real work is available. */
static void idle_task_step(void) {
    idle_tick_count++;
    __asm__ volatile("hlt");
}

static unsigned int interrupt_save(void) {
    unsigned int flags;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static void interrupt_restore(unsigned int flags) {
    if (flags & 0x200) {
        __asm__ volatile ("sti" : : : "memory");
    }
}

static void copy_text(char* dest, const char* src, int max_length) {
    int i = 0;

    if (max_length <= 0) {
        return;
    }

    if (src == 0) {
        dest[0] = '\0';
        return;
    }

    while (src[i] != '\0' && i < max_length - 1) {
        dest[i] = src[i];
        i++;
    }

    dest[i] = '\0';
}

/* POSIX status encoding:
 *   normal exit  -> (exit_code & 0xFF) << 8
 *   signal death -> signum & 0x7F  (bit 7 stays 0)
 */
static int encode_child_status(const task_entry_t* t) {
    if (!t) return 0;
    if (t->exit_code >= 128)
        return (t->exit_code - 128) & 0x7F;   /* killed by signal */
    return (t->exit_code & 0xFF) << 8;         /* normal exit */
}

/* Write a 32-bit int into a task's user-space at virtual address 'vaddr'.
 * Uses the physical mapping so it works from any execution context.
 */
static void write_user_int32(task_entry_t* t, uint32_t vaddr, int value) {
    uint32_t base   = PAGING_USER_BASE;
    uint32_t size   = PAGING_USER_SIZE;
    uint32_t offset;
    if (!t || !t->user_physical_base || !vaddr) return;
    if (vaddr < base || vaddr + 4U > base + size) return;
    if (t->page_directory != 0 &&
        !paging_directory_user_buffer_is_accessible(t->page_directory, vaddr, sizeof(int))) {
        return;
    }
    offset = vaddr - base;
    *(int*)(uintptr_t)(t->user_physical_base + offset) = value;
}

static int signal_is_valid(int signum) {
    return signum > 0 && signum <= LYTH_SIGNAL_MAX;
}

static int signal_is_fatal_by_default(int signum) {
    switch (signum) {
        case LYTH_SIGCHLD:
            return 0;
        default:
            return 1;
    }
}

static int signal_default_is_ignore(int signum) {
    switch (signum) {
        case LYTH_SIGCHLD:
            return 1;
        default:
            return 0;
    }
}

static unsigned int signal_bit(int signum) {
    return (signum > 0 && signum <= LYTH_SIGNAL_MAX) ? (1U << signum) : 0U;
}

static unsigned int signal_mask_sanitize(unsigned int mask) {
    /* SIGKILL must never be blocked. */
    return mask & ~signal_bit(LYTH_SIGKILL);
}

static int signal_is_blocked(const task_entry_t* task, int signum) {
    unsigned int bit;

    if (task == 0) {
        return 0;
    }

    if (signum == LYTH_SIGKILL) {
        return 0;
    }

    bit = signal_bit(signum);
    return bit != 0U && (task->signal_mask & bit) != 0U;
}

static void task_install_signal_trampoline(task_entry_t* task) {
    static const unsigned char code[] = { 0x83, 0xC4, 0x04, 0xC3 }; /* add $4,%esp ; ret */
    uint32_t va;
    uint32_t offset;
    unsigned char* dst;

    if (task == 0 || !task->user_mode || task->user_physical_base == 0) {
        if (task != 0) {
            task->signal_trampoline_va = 0;
        }
        return;
    }

    va = PAGING_USER_STACK_TOP - PAGING_USER_SIGNAL_TRAMPOLINE_SIZE;
    offset = va - PAGING_USER_BASE;
    if (offset + sizeof(code) > PAGING_USER_SIZE) {
        task->signal_trampoline_va = 0;
        return;
    }

    dst = (unsigned char*)(uintptr_t)(task->user_physical_base + offset);
    for (unsigned int i = 0; i < sizeof(code); i++) {
        dst[i] = code[i];
    }

    task->signal_trampoline_va = va;
}

static void task_signal_handlers_init(task_entry_t* task) {
    if (task == 0) {
        return;
    }

    task->pending_signals = 0;
    task->signal_mask = 0;
    for (int i = 0; i <= LYTH_SIGNAL_MAX; i++) {
        task->signal_handlers[i] = LYTH_SIG_DFL;
    }
    task->signal_trampoline_va = 0;
}

static void task_signal_handlers_copy(task_entry_t* dst, const task_entry_t* src) {
    if (dst == 0 || src == 0) {
        return;
    }

    dst->pending_signals = 0;
    dst->signal_mask = signal_mask_sanitize(src->signal_mask);
    for (int i = 0; i <= LYTH_SIGNAL_MAX; i++) {
        dst->signal_handlers[i] = src->signal_handlers[i];
    }
    dst->signal_handlers[LYTH_SIGKILL] = LYTH_SIG_DFL;
    dst->signal_trampoline_va = src->signal_trampoline_va;
}

static task_entry_t* find_task_by_id(int id) {
    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].id == id) {
            return &tasks[i];
        }
    }

    return 0;
}

static void clear_task(task_entry_t* task) {
    if (task == 0) {
        return;
    }

    sched_remove_from_ready(task_slot_index(task));

    /* Close all open file descriptors before releasing other resources. */
    vfs_task_fd_close_all(task->fd_table);

    if (task->data != 0) {
        kfree(task->data);
    }

    if (task->stack != 0) {
        kfree(task->stack);
    }

    if (task->user_stack != 0 && task->user_mode == 0) {
        kfree(task->user_stack);
    }

    if (task->page_directory != 0) {
        shm_detach_all(task->page_directory, task->shm_mappings, SHM_MAX_MAPPINGS);
        paging_destroy_user_directory(task->page_directory);
    }

    if (task->user_mode && task->user_physical_base != 0) {
        physmem_free_region(task->user_physical_base, PAGING_USER_SIZE);
    }

    task->used = 0;
    task->id = 0;
    task->name[0] = '\0';
    task->state = TASK_STATE_FREE;
    task->foreground = 0;
    task->cancel_requested = 0;
    task->wake_tick = 0;
    task->blocked_event_id = -1;
    task->priority = TASK_PRIORITY_NORMAL;
    task->step = 0;
    task->data = 0;
    task->data_size = 0;
    task->exit_code = 0;
    task->saved_esp = 0;
    task->stack = 0;
    task->user_stack = 0;
    task->user_entry = 0;
    task->user_mode = 0;
    task->user_physical_base = 0;
    task->user_heap_base = 0;
    task->user_heap_size = 0;
    task->user_initial_esp = 0;
    task->page_directory = 0;
    task->k_errno = 0;
    task->parent_id = 0;
    task->waitpid_status_uptr = 0;
    task->sched_next = -1;
    task->pending_signals = 0;
    task->signal_mask = 0;
    task->signal_trampoline_va = 0;
    task->uid = 0U;
    task->gid = 0U;
    task->euid = 0U;
    task->egid = 0U;
    task->supp_group_count = 0;
    for (int i = 0; i < TASK_MAX_SUPP_GROUPS; i++) {
        task->supp_groups[i] = 0U;
    }
    for (int i = 0; i <= LYTH_SIGNAL_MAX; i++) {
        task->signal_handlers[i] = LYTH_SIG_DFL;
    }
    task->alarm_tick = 0;
    task->itimer_interval_ticks = 0;
    task_reset_shm_mappings(task);
}

/*
 * zombify_task: free all resources of a finished task but keep the slot so
 * the parent can collect the exit code.  All freed pointers are NULLed so
 * a subsequent clear_task() is safe (it skips NULL pointers).
 */
static void zombify_task(task_entry_t* task) {
    if (task == 0) return;

    sched_remove_from_ready(task_slot_index(task));

    vfs_task_fd_close_all(task->fd_table);

    if (task->data != 0) {
        kfree(task->data);
        task->data = 0;
    }
    if (task->stack != 0) {
        kfree(task->stack);
        task->stack = 0;
    }
    /* user_stack for kernel-mode tasks is heap-allocated; for user-mode tasks
       it lives inside user_physical_base, so we must NOT kfree it there. */
    if (task->user_stack != 0 && task->user_mode == 0) {
        kfree(task->user_stack);
        task->user_stack = 0;
    }
    if (task->page_directory != 0) {
        shm_detach_all(task->page_directory, task->shm_mappings, SHM_MAX_MAPPINGS);
        paging_destroy_user_directory(task->page_directory);
        task->page_directory = 0;
    }
    if (task->user_mode && task->user_physical_base != 0) {
        physmem_free_region(task->user_physical_base, PAGING_USER_SIZE);
        task->user_physical_base = 0;
    }

    task->state = TASK_STATE_ZOMBIE;
    /* Preserved: used, id, name, exit_code, parent_id */
}

/* Reassign all living/zombie children of old_parent_id to new_parent_id. */
static void reparent_children(int old_parent_id, int new_parent_id) {
    int i;

    if (old_parent_id <= 0) return;

    for (i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].parent_id == old_parent_id) {
            tasks[i].parent_id = new_parent_id;
        }
    }
}

static int task_queue_signal_locked(task_entry_t* target, int signum) {
    if (target == 0 || !target->used || !signal_is_valid(signum)) {
        return 0;
    }

    if (target->state == TASK_STATE_ZOMBIE) {
        return 0;
    }

    /* Idle task is indestructible */
    if (target->id == idle_task_id) {
        return 0;
    }

    if (signum == LYTH_SIGKILL) {
        target->cancel_requested = 1;
        target->exit_code = 128 + signum;

        if (target->state == TASK_STATE_RUNNING) {
            target->state = TASK_STATE_CANCELLED;
            return 1;
        }

        target->state = TASK_STATE_CANCELLED;
        complete_task(target);
        return 1;
    }

    /* Coalesced pending semantics: one bit per signal number. */
    target->pending_signals |= signal_bit(signum);

    if (target->state == TASK_STATE_SLEEPING || target->state == TASK_STATE_BLOCKED) {
        target->blocked_event_id = -1;
        target->state = TASK_STATE_READY;
        sched_enqueue_ready(task_slot_index(target));
    }

    return 1;
}

static int task_pop_pending_signal(task_entry_t* task) {
    if (task == 0) {
        return 0;
    }

    for (int signum = 1; signum <= LYTH_SIGNAL_MAX; signum++) {
        unsigned int bit = signal_bit(signum);
        if ((task->pending_signals & bit) && !signal_is_blocked(task, signum)) {
            task->pending_signals &= ~bit;
            return signum;
        }
    }

    return 0;
}

static int task_push_user_signal_frame(task_entry_t* task,
                                       task_user_stack_frame_t* frame,
                                       unsigned int handler,
                                       int signum) {
    uint32_t new_user_esp;
    uint32_t offset;
    uint32_t* sp;

    if (task == 0 || frame == 0 || handler <= LYTH_SIG_IGN || task->signal_trampoline_va == 0) {
        return 0;
    }

    if (frame->user_esp < (PAGING_USER_STACK_BOTTOM + 12U) ||
        frame->user_esp > PAGING_USER_STACK_TOP) {
        return 0;
    }

    new_user_esp = frame->user_esp - 12U;
    if (new_user_esp < PAGING_USER_STACK_BOTTOM) {
        return 0;
    }

    offset = new_user_esp - PAGING_USER_BASE;
    if (offset + 12U > PAGING_USER_SIZE) {
        return 0;
    }

    sp = (uint32_t*)(uintptr_t)(task->user_physical_base + offset);
    /* cdecl frame for handler(int):
       [esp+0]  = return address -> trampoline
       [esp+4]  = signum argument
       [esp+8]  = original eip (consumed by trampoline ret)
     */
    sp[0] = task->signal_trampoline_va;
    sp[1] = (uint32_t)signum;
    sp[2] = frame->eip;

    frame->user_esp = new_user_esp;
    frame->eip = handler;
    return 1;
}

static void task_deliver_signals_current(task_entry_t* task, unsigned int frame_esp) {
    task_user_stack_frame_t* frame;
    int signum;
    unsigned int handler;

    if (task == 0 || !task->used || !task->user_mode || task->state != TASK_STATE_RUNNING) {
        return;
    }

    signum = task_pop_pending_signal(task);
    if (signum == 0) {
        return;
    }

    handler = task->signal_handlers[signum];

    if (handler == LYTH_SIG_IGN) {
        return;
    }

    if (handler == LYTH_SIG_DFL || signum == LYTH_SIGKILL) {
        if (signal_default_is_ignore(signum)) {
            return;
        }
        if (signal_is_fatal_by_default(signum) || signum == LYTH_SIGKILL) {
            task->cancel_requested = 1;
            task->exit_code = 128 + signum;
            task->state = TASK_STATE_CANCELLED;
        }
        return;
    }

    frame = (task_user_stack_frame_t*)frame_esp;
    if (!task_push_user_signal_frame(task, frame, handler, signum)) {
        task->cancel_requested = 1;
        task->exit_code = 128 + signum;
        task->state = TASK_STATE_CANCELLED;
    }
}

static void complete_task(task_entry_t* task) {
    int task_id;
    const char* task_name;
    int cancelled;
    int notify_foreground;
    int parent_id;
    task_entry_t* parent;

    if (task == 0 || !task->used) {
        return;
    }

    /* Protect the idle task: it must never terminate. Re-enqueue it. */
    if (task->id == idle_task_id) {
        task->state = TASK_STATE_READY;
        sched_enqueue_ready(task_slot_index(task));
        return;
    }

    sched_remove_from_ready(task_slot_index(task));

    task_id = task->id;
    task_name = task->name;
    cancelled = task->cancel_requested || task->state == TASK_STATE_CANCELLED;
    notify_foreground = task->foreground && foreground_task_id == task->id;
    parent_id = task->parent_id;

    if (notify_foreground) {
        foreground_task_id = -1;
    }

    if (notify_foreground && foreground_complete_handler != 0) {
        foreground_complete_handler(task_id, task_name, cancelled);
    }

    if (!cancelled &&
        task_name[0] == 's' && task_name[1] == 't' && task_name[2] == 'a' &&
        task_name[3] == 'c' && task_name[4] == 'k' && task_name[5] == 'o' &&
        task_name[6] == 'k' && task_name[7] == '\0') {
        terminal_print("[job ");
        terminal_print_uint((unsigned int)task_id);
        terminal_print("] ");
        terminal_print(task_name);
        terminal_print_line(" terminado correctamente");
    }

    if (task_name[0] == 's' && task_name[1] == 'h' && task_name[2] == 'm' &&
        task_name[3] == 'r' && task_name[4] == 'e' && task_name[5] == 'a' &&
        task_name[6] == 'd' && task_name[7] == '\0') {
        terminal_print("[job ");
        terminal_print_uint((unsigned int)task_id);
        terminal_print("] shmread ");
        if (!cancelled && task->exit_code == 0) {
            terminal_print_line("verificado correctamente");
        } else {
            terminal_print_line("fallo la validacion");
        }
    }

    /* Reparent any live children so they aren't orphaned when this task dies. */
    reparent_children(task_id, sys_init_pid);

    /* Wake any task blocked waiting for this specific task ID. */
    task_signal_event(task_id);

    parent = (parent_id > 0) ? find_task_by_id(parent_id) : 0;

    /* Notify parent with SIGCHLD for basic child-exit delivery semantics. */
    if (parent != 0 && parent->used && parent->id != task_id) {
        task_queue_signal_locked(parent, LYTH_SIGCHLD);
    }

    if (parent != 0 &&
        parent->state == TASK_STATE_BLOCKED &&
        (parent->blocked_event_id == task_id || parent->blocked_event_id == parent->id)) {
        /* Parent is actively waiting for this child (or any child):
           wake it and reap immediately. */

        /* Write child PID as the syscall return value into parent's saved frame. */
        if (parent->saved_esp != 0) {
            task_user_stack_frame_t* pframe =
                (task_user_stack_frame_t*)parent->saved_esp;
            pframe->eax = (unsigned int)task_id;
        }
        /* Write POSIX status to user-space pointer if set. */
        if (parent->waitpid_status_uptr) {
            write_user_int32(parent, parent->waitpid_status_uptr,
                             encode_child_status(task));
            parent->waitpid_status_uptr = 0;
        }

        task_signal_event(parent->blocked_event_id);
        clear_task(task);
        return;
    }

    /* Decide: zombify (parent alive) or clear immediately. */
    if (parent != 0 && parent->state != TASK_STATE_ZOMBIE) {
        /* Parent is still alive — become a zombie so it can collect exit status. */
        zombify_task(task);
        /* Also wake the parent in case it is blocking on wait_any_child. */
        task_signal_event(parent_id);
    } else {
        /* No living parent — reap immediately. */
        clear_task(task);
    }
}

static int select_next_ready_task(void) {
    int prio;
    for (prio = (int)TASK_PRIORITY_HIGH; prio < SCHED_PRIO_COUNT; prio++) {
        if (sched_ready_head[prio] >= 0) return sched_ready_head[prio];
    }
    return -1;
}

static unsigned int schedule_from_idle(unsigned int current_esp) {
    int selected = select_next_ready_task();

    if (selected < 0) {
        return current_esp;
    }

    sched_dequeue_ready((int)tasks[selected].priority);

    idle_context_esp = current_esp;
    current_task_index = selected;
    tasks[selected].state = TASK_STATE_RUNNING;
    ctx_switch_count++;

    terminal_set_output_vc(tasks[selected].output_vc);

    if (tasks[selected].page_directory != 0) {
        paging_switch_directory(tasks[selected].page_directory);
    } else {
        paging_switch_directory(paging_kernel_directory());
    }

    return tasks[selected].saved_esp;
}

static unsigned int schedule_back_to_idle(unsigned int current_esp) {
    task_entry_t* task;

    if (current_task_index < 0) {
        return current_esp;
    }

    task = &tasks[current_task_index];
    task->saved_esp = current_esp;

    terminal_set_output_vc(-1);

    paging_switch_directory(paging_kernel_directory());

    /* Re-enqueue if still runnable (RUNNING = preempted; READY = yielded). */
    if (task->state == TASK_STATE_RUNNING || task->state == TASK_STATE_READY) {
        task->state = TASK_STATE_READY;
        sched_enqueue_ready(current_task_index);
    }

    if (task->used &&
        (task->state == TASK_STATE_FINISHED || task->state == TASK_STATE_CANCELLED)) {
        complete_task(task);
    }

    current_task_index = -1;

    if (idle_context_esp != 0) {
        return idle_context_esp;
    }

    return current_esp;
}

static void initialize_task_stack(task_entry_t* task) {
    unsigned int stack_top;

    stack_top = (unsigned int)(task->stack + TASK_STACK_SIZE);

    if (task->user_mode) {
        task_user_stack_frame_t* frame;
        unsigned int user_stack_top;

        user_stack_top = (unsigned int)(task->user_stack + TASK_USER_STACK_SIZE);
        if (task->user_initial_esp != 0)
            user_stack_top = (unsigned int)task->user_initial_esp;
        else
            user_stack_top -= PAGING_USER_SIGNAL_TRAMPOLINE_SIZE;
        stack_top -= sizeof(task_user_stack_frame_t);
        frame = (task_user_stack_frame_t*)stack_top;

        frame->edi = 0;
        frame->esi = 0;
        frame->ebp = 0;
        frame->esp_placeholder = 0;
        frame->ebx = 0;
        frame->edx = 0;
        frame->ecx = 0;
        frame->eax = 0;
        frame->eip = task->user_entry;
        frame->cs = user_code_selector | 0x03;
        frame->eflags = 0x00000202U;
        frame->user_esp = user_stack_top;
        frame->user_ss = user_data_selector | 0x03;
    } else {
        task_stack_frame_t* frame;

        stack_top -= sizeof(task_stack_frame_t);
        frame = (task_stack_frame_t*)stack_top;

        frame->edi = 0;
        frame->esi = 0;
        frame->ebp = 0;
        frame->esp_placeholder = 0;
        frame->ebx = 0;
        frame->edx = 0;
        frame->ecx = 0;
        frame->eax = 0;
        frame->eip = (unsigned int)(uintptr_t)task_thread_bootstrap;
        frame->cs = kernel_code_selector;
        frame->eflags = 0x00000202U;
    }

    task->saved_esp = stack_top;
}

static void task_thread_bootstrap(void) {
    for (;;) {
        task_entry_t* task;

        if (current_task_index < 0) {
            syscall_yield();
            continue;
        }

        task = &tasks[current_task_index];

        if (!task->used || task->step == 0) {
            task_exit(1);
            syscall_yield();
            continue;
        }

        task->step();

        if (current_task_index < 0) {
            continue;
        }

        if (tasks[current_task_index].state != TASK_STATE_RUNNING) {
            syscall_yield();
        }
    }
}

void task_system_init(void) {
    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        tasks[i].used = 0;
        tasks[i].data = 0;
        tasks[i].stack = 0;
        tasks[i].user_stack = 0;
        tasks[i].page_directory = 0;
        tasks[i].user_physical_base = 0;
        tasks[i].user_heap_base = 0;
        tasks[i].user_heap_size = 0;
        tasks[i].waitpid_status_uptr = 0;
        tasks[i].sched_next = -1;
        vfs_task_fd_init(tasks[i].fd_table);
        task_signal_handlers_init(&tasks[i]);
        task_reset_shm_mappings(&tasks[i]);
    }

    shm_init();
    mqueue_init();

    current_task_index = -1;
    next_task_id = 1;
    foreground_task_id = -1;
    idle_context_esp = 0;
    foreground_complete_handler = 0;
    idle_task_id   = -1;
    idle_tick_count  = 0;
    ctx_switch_count = 0;

    for (int p = 0; p < SCHED_PRIO_COUNT; p++) {
        sched_ready_head[p] = -1;
        sched_ready_tail[p] = -1;
    }

    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_code_selector));
    user_code_selector = gdt_user_code_selector();
    user_data_selector = gdt_user_data_selector();

    /* Spawn the system idle task — always runnable, runs HLT when nothing else
     * is ready.  Uses TASK_PRIORITY_IDLE so it is selected last.             */
    {
        int slot = -1;
        int i;
        for (i = 0; i < TASK_MAX_COUNT; i++) {
            if (!tasks[i].used) { slot = i; break; }
        }
        if (slot >= 0) {
            tasks[slot].used             = 1;
            tasks[slot].id               = next_task_id++;
            copy_text(tasks[slot].name, "[idle]", TASK_NAME_MAX);
            tasks[slot].state            = TASK_STATE_READY;
            tasks[slot].foreground       = 0;
            tasks[slot].cancel_requested = 0;
            tasks[slot].wake_tick        = 0;
            tasks[slot].blocked_event_id = -1;
            tasks[slot].priority         = TASK_PRIORITY_IDLE;
            tasks[slot].step             = idle_task_step;
            tasks[slot].data             = 0;
            tasks[slot].data_size        = 0;
            tasks[slot].exit_code        = 0;
            tasks[slot].saved_esp        = 0;
            tasks[slot].user_mode        = 0;
            tasks[slot].user_physical_base = 0;
            tasks[slot].user_heap_base   = 0;
            tasks[slot].user_heap_size   = 0;
            tasks[slot].user_initial_esp = 0;
            tasks[slot].page_directory   = 0;
            tasks[slot].k_errno          = 0;
            tasks[slot].parent_id        = 0;
            tasks[slot].waitpid_status_uptr = 0;
            tasks[slot].sched_next       = -1;
            tasks[slot].output_vc        = -1;
            task_signal_handlers_init(&tasks[slot]);
            vfs_task_fd_init(tasks[slot].fd_table);
            tasks[slot].stack = (unsigned char*)kmalloc(TASK_STACK_SIZE);
            if (tasks[slot].stack != 0) {
                initialize_task_stack(&tasks[slot]);
                sched_enqueue_ready(slot);
                idle_task_id = tasks[slot].id;
            } else {
                tasks[slot].used = 0;
            }
        }
    }

    KASSERT_MSG(idle_task_id != -1, "idle task creation failed");
}

int task_spawn(const char* name, task_step_fn step, const void* data, unsigned int data_size, int foreground) {
    unsigned int flags;
    int slot = -1;

    if (step == 0) {
        return -1;
    }

    flags = interrupt_save();

    if (foreground && foreground_task_id != -1) {
        interrupt_restore(flags);
        return -1;
    }

    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (!tasks[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        interrupt_restore(flags);
        return -1;
    }

    tasks[slot].used = 1;
    tasks[slot].id = next_task_id++;
    copy_text(tasks[slot].name, name, TASK_NAME_MAX);
    tasks[slot].state = TASK_STATE_READY;
    tasks[slot].foreground = foreground;
    tasks[slot].cancel_requested = 0;
    tasks[slot].wake_tick = 0;
    tasks[slot].blocked_event_id = -1;
    tasks[slot].priority = foreground ? TASK_PRIORITY_NORMAL : TASK_PRIORITY_LOW;
    tasks[slot].step = step;
    tasks[slot].data = 0;
    tasks[slot].data_size = data_size;
    tasks[slot].exit_code = 0;
    tasks[slot].output_vc = terminal_active_vc();
    tasks[slot].saved_esp = 0;
    tasks[slot].stack = 0;
    tasks[slot].user_stack = 0;
    tasks[slot].user_entry = 0;
    tasks[slot].user_mode = 0;
    tasks[slot].parent_id = (current_task_index >= 0) ? tasks[current_task_index].id : 0;
    tasks[slot].waitpid_status_uptr = 0;
    tasks[slot].sched_next = -1;
    tasks[slot].fd_limit_soft = RLIM_NOFILE_DEFAULT;
    tasks[slot].fd_limit_hard = RLIM_NOFILE_DEFAULT;
    /* Inherit uid/gid from parent; default to root for very first tasks */
    if (current_task_index >= 0) {
        tasks[slot].uid  = tasks[current_task_index].uid;
        tasks[slot].gid  = tasks[current_task_index].gid;
        tasks[slot].euid = tasks[current_task_index].euid;
        tasks[slot].egid = tasks[current_task_index].egid;
        tasks[slot].supp_group_count = tasks[current_task_index].supp_group_count;
        for (int gi = 0; gi < TASK_MAX_SUPP_GROUPS; gi++) {
            tasks[slot].supp_groups[gi] = tasks[current_task_index].supp_groups[gi];
        }
    } else {
        tasks[slot].uid = tasks[slot].gid = tasks[slot].euid = tasks[slot].egid = 0U;
        tasks[slot].supp_group_count = 0;
        for (int gi = 0; gi < TASK_MAX_SUPP_GROUPS; gi++) tasks[slot].supp_groups[gi] = 0U;
    }
    task_signal_handlers_init(&tasks[slot]);
    task_reset_shm_mappings(&tasks[slot]);
    vfs_task_fd_init(tasks[slot].fd_table);  /* (re-)install stdio if tty is ready */
    tasks[slot].stack = (unsigned char*)kmalloc(TASK_STACK_SIZE);
    if (tasks[slot].stack == 0) {
        clear_task(&tasks[slot]);
        interrupt_restore(flags);
        return -1;
    }

    if (data_size > 0) {
        unsigned char* dst = (unsigned char*)kmalloc(data_size);
        const unsigned char* src = (const unsigned char*)data;

        if (dst == 0) {
            clear_task(&tasks[slot]);
            interrupt_restore(flags);
            return -1;
        }

        for (unsigned int i = 0; i < data_size; i++) {
            dst[i] = src[i];
        }

        tasks[slot].data = dst;
    }

    initialize_task_stack(&tasks[slot]);

    if (foreground) {
        foreground_task_id = tasks[slot].id;
    }

    sched_enqueue_ready(slot);
    interrupt_restore(flags);
    return tasks[slot].id;
}

static int task_spawn_user_common(const char* name,
                                  unsigned int entry_point,
                                  uint32_t user_physical_base,
                                  uint32_t user_heap_base,
                                  uint32_t user_heap_size,
                                  uint32_t* page_directory,
                                  uint32_t initial_user_esp,
                                  int shm_segment_id,
                                  int foreground) {
    unsigned int flags;
    int slot = -1;

    if (entry_point == 0 || user_physical_base == 0 || page_directory == 0) {
        return -1;
    }

    flags = interrupt_save();

    if (foreground && foreground_task_id != -1) {
        interrupt_restore(flags);
        return -1;
    }

    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (!tasks[i].used) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        interrupt_restore(flags);
        return -1;
    }

    tasks[slot].used = 1;
    tasks[slot].id = next_task_id++;
    copy_text(tasks[slot].name, name, TASK_NAME_MAX);
    tasks[slot].state = TASK_STATE_READY;
    tasks[slot].foreground = foreground;
    tasks[slot].cancel_requested = 0;
    tasks[slot].wake_tick = 0;
    tasks[slot].blocked_event_id = -1;
    tasks[slot].priority = foreground ? TASK_PRIORITY_NORMAL : TASK_PRIORITY_LOW;
    tasks[slot].step = 0;
    tasks[slot].data = 0;
    tasks[slot].data_size = 0;
    tasks[slot].exit_code = 0;
    tasks[slot].output_vc = terminal_active_vc();
    tasks[slot].saved_esp = 0;
    tasks[slot].stack = (unsigned char*)kmalloc(TASK_STACK_SIZE);
    tasks[slot].user_stack = 0;
    tasks[slot].user_entry = entry_point;
    tasks[slot].user_mode = 1;
    tasks[slot].user_physical_base = user_physical_base;
    tasks[slot].user_heap_base = user_heap_base;
    tasks[slot].user_heap_size = user_heap_size;
    tasks[slot].page_directory = page_directory;
    tasks[slot].user_initial_esp = initial_user_esp;
    tasks[slot].parent_id = (current_task_index >= 0) ? tasks[current_task_index].id : 0;
    tasks[slot].waitpid_status_uptr = 0;
    tasks[slot].sched_next = -1;
    tasks[slot].fd_limit_soft = RLIM_NOFILE_DEFAULT;
    tasks[slot].fd_limit_hard = RLIM_NOFILE_DEFAULT;
    if (current_task_index >= 0) {
        tasks[slot].uid  = tasks[current_task_index].uid;
        tasks[slot].gid  = tasks[current_task_index].gid;
        tasks[slot].euid = tasks[current_task_index].euid;
        tasks[slot].egid = tasks[current_task_index].egid;
        tasks[slot].supp_group_count = tasks[current_task_index].supp_group_count;
        for (int gi = 0; gi < TASK_MAX_SUPP_GROUPS; gi++) {
            tasks[slot].supp_groups[gi] = tasks[current_task_index].supp_groups[gi];
        }
    } else {
        tasks[slot].uid = tasks[slot].gid = tasks[slot].euid = tasks[slot].egid = 0U;
        tasks[slot].supp_group_count = 0;
        for (int gi = 0; gi < TASK_MAX_SUPP_GROUPS; gi++) tasks[slot].supp_groups[gi] = 0U;
    }
    task_signal_handlers_init(&tasks[slot]);
    task_reset_shm_mappings(&tasks[slot]);

    vfs_task_fd_init(tasks[slot].fd_table);  /* (re-)install stdio if tty is ready */

    if (tasks[slot].stack == 0) {
        clear_task(&tasks[slot]);
        interrupt_restore(flags);
        return -1;
    }

    tasks[slot].user_stack = (unsigned char*)(uintptr_t)PAGING_USER_STACK_BOTTOM;
    task_install_signal_trampoline(&tasks[slot]);

    if (shm_segment_id > 0 &&
        shm_attach(tasks[slot].page_directory,
                   tasks[slot].shm_mappings,
                   SHM_MAX_MAPPINGS,
                   shm_segment_id) == 0U) {
        clear_task(&tasks[slot]);
        interrupt_restore(flags);
        return -1;
    }

    initialize_task_stack(&tasks[slot]);

    if (foreground) {
        foreground_task_id = tasks[slot].id;
    }

    sched_enqueue_ready(slot);
    interrupt_restore(flags);
    return tasks[slot].id;
}

int task_spawn_user(const char* name,
                    unsigned int entry_point,
                    uint32_t user_physical_base,
                    uint32_t user_heap_base,
                    uint32_t user_heap_size,
                    uint32_t* page_directory,
                    uint32_t initial_user_esp,
                    int foreground) {
    return task_spawn_user_common(name,
                                  entry_point,
                                  user_physical_base,
                                  user_heap_base,
                                  user_heap_size,
                                  page_directory,
                                  initial_user_esp,
                                  -1,
                                  foreground);
}

int task_spawn_user_shm1(const char* name,
                         unsigned int entry_point,
                         uint32_t user_physical_base,
                         uint32_t user_heap_base,
                         uint32_t user_heap_size,
                         uint32_t* page_directory,
                         uint32_t initial_user_esp,
                         int shm_segment_id,
                         int foreground) {
    return task_spawn_user_common(name,
                                  entry_point,
                                  user_physical_base,
                                  user_heap_base,
                                  user_heap_size,
                                  page_directory,
                                  initial_user_esp,
                                  shm_segment_id,
                                  foreground);
}

void task_on_tick(void) {
    unsigned int now = timer_get_ticks();

    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (!tasks[i].used) continue;

        if (tasks[i].state == TASK_STATE_SLEEPING) {
            if ((int)(now - tasks[i].wake_tick) >= 0) {
                tasks[i].wake_tick = 0U;
                tasks[i].state = TASK_STATE_READY;
                sched_enqueue_ready(i);
            }
        }

        if (tasks[i].state == TASK_STATE_BLOCKED &&
            tasks[i].wake_tick != 0U &&
            (int)(now - tasks[i].wake_tick) >= 0) {
            tasks[i].wake_tick = 0U;
            tasks[i].blocked_event_id = -1;
            tasks[i].state = TASK_STATE_READY;
            sched_enqueue_ready(i);
        }

        /* ITIMER_REAL / alarm(): fire SIGALRM when deadline reached */
        if (tasks[i].alarm_tick != 0 && (int)(now - tasks[i].alarm_tick) >= 0) {
            if (tasks[i].itimer_interval_ticks != 0) {
                /* Periodic: reload */
                tasks[i].alarm_tick = now + tasks[i].itimer_interval_ticks;
            } else {
                /* One-shot: disarm */
                tasks[i].alarm_tick = 0;
            }
            /* Deliver SIGALRM regardless of user handler (wakes sleeping tasks too) */
            tasks[i].pending_signals |= (1U << LYTH_SIGALRM);
            if (tasks[i].state == TASK_STATE_SLEEPING ||
                tasks[i].state == TASK_STATE_BLOCKED) {
                tasks[i].blocked_event_id = -1;
                tasks[i].state = TASK_STATE_READY;
                sched_enqueue_ready(i);
            }
        }
    }
}

void task_run_ready(void) {
    (void)0;
}

int task_has_runnable(void) {
    int p;
    for (p = (int)TASK_PRIORITY_HIGH; p <= (int)TASK_PRIORITY_LOW; p++) {
        if (sched_ready_head[p] >= 0) return 1;
    }
    return 0;
}

void task_sleep(unsigned int ticks) {
    if (current_task_index < 0) {
        return;
    }

    if (ticks == 0) {
        tasks[current_task_index].state = TASK_STATE_READY;
        return;
    }

    tasks[current_task_index].wake_tick = timer_get_ticks() + ticks;
    tasks[current_task_index].blocked_event_id = -1;
    tasks[current_task_index].state = TASK_STATE_SLEEPING;
}

void task_wait_event(int event_id) {
    int i;

    if (current_task_index < 0 || event_id < 0) {
        return;
    }

    /* If event_id matches a zombie task's PID, don't block — it already finished. */
    for (i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].id == event_id &&
                tasks[i].state == TASK_STATE_ZOMBIE) {
            return;
        }
    }

    tasks[current_task_index].blocked_event_id = event_id;
    tasks[current_task_index].wake_tick = 0U;
    tasks[current_task_index].state = TASK_STATE_BLOCKED;
}

void task_wait_event_timeout(int event_id, unsigned int timeout_ticks) {
    int i;

    if (current_task_index < 0 || event_id < 0) {
        return;
    }

    if (timeout_ticks == 0U) {
        task_wait_event(event_id);
        return;
    }

    for (i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].id == event_id &&
                tasks[i].state == TASK_STATE_ZOMBIE) {
            return;
        }
    }

    tasks[current_task_index].blocked_event_id = event_id;
    tasks[current_task_index].wake_tick = timer_get_ticks() + timeout_ticks;
    tasks[current_task_index].state = TASK_STATE_BLOCKED;
}

int task_signal_event(int event_id) {
    int woken = 0;
    unsigned int flags;

    if (event_id < 0) {
        return 0;
    }

    flags = interrupt_save();

    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used &&
            tasks[i].state == TASK_STATE_BLOCKED &&
            tasks[i].blocked_event_id == event_id) {
            tasks[i].blocked_event_id = -1;
            tasks[i].wake_tick = 0U;
            tasks[i].state = TASK_STATE_READY;
            sched_enqueue_ready(i);
            woken++;
        }
    }

    interrupt_restore(flags);
    return woken;
}

void task_yield(void) {
    if (current_task_index < 0) {
        return;
    }

    if (tasks[current_task_index].state == TASK_STATE_RUNNING) {
        tasks[current_task_index].state = TASK_STATE_READY;
    }
}

void task_exit(int exit_code) {
    if (current_task_index < 0) {
        return;
    }

    tasks[current_task_index].exit_code = exit_code;
    tasks[current_task_index].state = TASK_STATE_FINISHED;
}

void task_request_cancel(void) {
    task_entry_t* task = 0;

    if (current_task_index >= 0) {
        task = &tasks[current_task_index];
    }

    if (task == 0) {
        return;
    }

    task->cancel_requested = 1;
    if (task->state == TASK_STATE_SLEEPING || task->state == TASK_STATE_BLOCKED) {
        task->blocked_event_id = -1;
        task->state = TASK_STATE_READY;
        sched_enqueue_ready(task_slot_index(task));
    }
}

void task_request_foreground_cancel(void) {
    unsigned int flags;
    task_entry_t* task;

    flags = interrupt_save();

    if (foreground_task_id == -1) {
        interrupt_restore(flags);
        return;
    }

    task = find_task_by_id(foreground_task_id);
    if (task == 0) {
        interrupt_restore(flags);
        return;
    }

    task->cancel_requested = 1;
    if (task->state == TASK_STATE_SLEEPING || task->state == TASK_STATE_BLOCKED) {
        task->blocked_event_id = -1;
        task->state = TASK_STATE_READY;
        sched_enqueue_ready(task_slot_index(task));
    }

    interrupt_restore(flags);
}

int task_cancel_requested(void) {
    if (current_task_index >= 0) {
        return tasks[current_task_index].cancel_requested;
    }

    if (foreground_task_id != -1) {
        task_entry_t* task = find_task_by_id(foreground_task_id);
        return task != 0 ? task->cancel_requested : 0;
    }

    return 0;
}

void task_clear_cancel(void) {
    if (current_task_index >= 0) {
        tasks[current_task_index].cancel_requested = 0;
        return;
    }

    if (foreground_task_id != -1) {
        task_entry_t* task = find_task_by_id(foreground_task_id);
        if (task != 0) {
            task->cancel_requested = 0;
        }
    }
}

int task_is_running(void) {
    return current_task_index >= 0;
}

int task_current_is_user_mode(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].user_mode;
}

uint32_t task_current_user_heap_base(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].user_heap_base;
}

uint32_t task_current_user_heap_size(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].user_heap_size;
}

task_priority_t task_current_priority(void) {
    if (current_task_index < 0) {
        return TASK_PRIORITY_NORMAL;
    }

    return tasks[current_task_index].priority;
}

const char* task_current_name(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].name;
}

int task_current_id(void) {
    if (current_task_index < 0) {
        return -1;
    }

    return tasks[current_task_index].id;
}

vfs_fd_entry_t* task_current_fd_table(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].fd_table;
}

vfs_fd_entry_t* task_get_fd_table(int task_id) {
    task_entry_t* t = find_task_by_id(task_id);
    return t ? t->fd_table : 0;
}

void* task_current_data(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].data;
}

int task_has_foreground_task(void) {
    return foreground_task_id != -1;
}

int task_foreground_task_id(void) {
    return foreground_task_id;
}

int task_set_priority(int id, task_priority_t priority) {
    unsigned int flags;
    task_entry_t* task;
    int idx;
    task_priority_t old_priority;

    if (priority < TASK_PRIORITY_HIGH || priority > TASK_PRIORITY_LOW) {
        return 0;
    }

    flags = interrupt_save();
    task = find_task_by_id(id);

    if (task == 0) {
        interrupt_restore(flags);
        return 0;
    }

    if (task->id == idle_task_id) {
        interrupt_restore(flags);
        return 0;
    }

    idx = task_slot_index(task);
    old_priority = task->priority;

    if (old_priority == priority) {
        interrupt_restore(flags);
        return 1;
    }

    if (task->state == TASK_STATE_READY) {
        sched_remove_from_ready(idx);
        task->priority = priority;
        sched_enqueue_ready(idx);
    } else {
        task->priority = priority;
    }

    interrupt_restore(flags);
    return 1;
}

void task_set_output_vc(int id, int vc_index) {
    unsigned int flags = interrupt_save();
    task_entry_t* task = find_task_by_id(id);
    if (task != 0) {
        task->output_vc = vc_index;
    }
    interrupt_restore(flags);
}

const char* task_priority_name(task_priority_t priority) {
    switch (priority) {
        case TASK_PRIORITY_HIGH:
            return "HIGH";
        case TASK_PRIORITY_IDLE:
            return "IDLE";
        case TASK_PRIORITY_LOW:
            return "LOW";
        default:
            return "NORMAL";
    }
}

void task_set_foreground_complete_handler(void (*handler)(int id, const char* name, int cancelled)) {
    foreground_complete_handler = handler;
}

/*
 * task_alarm: arm a one-shot SIGALRM for the current task after `seconds`.
 * seconds==0 cancels any pending alarm.
 * Returns the number of seconds remaining in the previous alarm (POSIX).
 */
unsigned int task_alarm(unsigned int seconds) {
    unsigned int flags;
    unsigned int freq;
    unsigned int now;
    unsigned int remaining = 0;

    if (current_task_index < 0) return 0;

    flags = interrupt_save();
    freq  = timer_get_frequency();
    now   = timer_get_ticks();

    if (tasks[current_task_index].alarm_tick != 0) {
        unsigned int left_ticks = tasks[current_task_index].alarm_tick - now;
        remaining = (freq > 0) ? (left_ticks + freq - 1U) / freq : 0;
    }

    if (seconds == 0) {
        tasks[current_task_index].alarm_tick = 0;
        tasks[current_task_index].itimer_interval_ticks = 0;
    } else {
        tasks[current_task_index].alarm_tick = now + seconds * freq;
        tasks[current_task_index].itimer_interval_ticks = 0; /* one-shot */
    }

    interrupt_restore(flags);
    return remaining;
}

/*
 * task_setitimer: arm/disarm ITIMER_REAL for the current task.
 * interval_us and value_us are in microseconds.
 * old_value_us / old_interval_us (if non-NULL) receive previous state.
 * Returns 0 on success, -1 on error.
 */
int task_setitimer(unsigned int value_us, unsigned int interval_us,
                   unsigned int* old_value_us_out, unsigned int* old_interval_us_out) {
    unsigned int flags;
    unsigned int freq;
    unsigned int now;
    unsigned int us_per_tick;

    if (current_task_index < 0) return -1;

    flags = interrupt_save();
    freq  = timer_get_frequency();
    now   = timer_get_ticks();
    us_per_tick = (freq > 0) ? (1000000U / freq) : 0U;

    if (old_value_us_out) {
        if (tasks[current_task_index].alarm_tick != 0 && freq > 0) {
            unsigned int left = tasks[current_task_index].alarm_tick - now;
            *old_value_us_out = (left * 1000000U) / freq;
        } else {
            *old_value_us_out = 0;
        }
    }
    if (old_interval_us_out) {
        unsigned int itv = tasks[current_task_index].itimer_interval_ticks;
        *old_interval_us_out = (freq > 0) ? (itv * 1000000U) / freq : 0;
    }

    if (value_us == 0) {
        tasks[current_task_index].alarm_tick = 0;
        tasks[current_task_index].itimer_interval_ticks = 0;
    } else {
        unsigned int value_ticks;
        unsigned int interval_ticks;
        if (us_per_tick == 0U) {
            interrupt_restore(flags);
            return -1;
        }
        value_ticks = (value_us + us_per_tick - 1U) / us_per_tick;
        interval_ticks = interval_us / us_per_tick;
        if (value_ticks == 0) value_ticks = 1;
        tasks[current_task_index].alarm_tick = now + value_ticks;
        tasks[current_task_index].itimer_interval_ticks = interval_ticks;
    }

    interrupt_restore(flags);
    return 0;
}

void task_getitimer(unsigned int* value_us_out, unsigned int* interval_us_out) {
    unsigned int flags;
    unsigned int freq;
    unsigned int now;

    if (!value_us_out && !interval_us_out) return;
    if (current_task_index < 0) {
        if (value_us_out)    *value_us_out    = 0;
        if (interval_us_out) *interval_us_out = 0;
        return;
    }

    flags = interrupt_save();
    freq  = timer_get_frequency();
    now   = timer_get_ticks();

    if (value_us_out) {
        if (tasks[current_task_index].alarm_tick != 0 && freq > 0) {
            unsigned int left = tasks[current_task_index].alarm_tick - now;
            *value_us_out = (left * 1000000U) / freq;
        } else {
            *value_us_out = 0;
        }
    }
    if (interval_us_out) {
        unsigned int itv = tasks[current_task_index].itimer_interval_ticks;
        *interval_us_out = (freq > 0) ? (itv * 1000000U) / freq : 0;
    }

    interrupt_restore(flags);
}

int task_set_foreground(int id) {
    unsigned int flags;
    task_entry_t* old_task;
    task_entry_t* new_task;

    flags = interrupt_save();

    /* Clear old foreground task */
    if (foreground_task_id != -1) {
        old_task = find_task_by_id(foreground_task_id);
        if (old_task)
            old_task->foreground = 0;
        foreground_task_id = -1;
    }

    /* id < 0 means "just clear FG, no new owner" */
    if (id < 0) {
        interrupt_restore(flags);
        return 1;
    }

    new_task = find_task_by_id(id);
    if (!new_task || new_task->state == TASK_STATE_FREE ||
        new_task->state == TASK_STATE_FINISHED ||
        new_task->state == TASK_STATE_ZOMBIE ||
        new_task->state == TASK_STATE_CANCELLED) {
        interrupt_restore(flags);
        return 0;
    }

    new_task->foreground = 1;
    foreground_task_id   = id;
    interrupt_restore(flags);
    return 1;
}

int task_list(task_snapshot_t* out, int max_tasks) {
    int count = 0;

    if (out == 0 || max_tasks <= 0) {
        return 0;
    }

    for (int i = 0; i < TASK_MAX_COUNT && count < max_tasks; i++) {
        if (!tasks[i].used) {
            continue;
        }

        out[count].id = tasks[i].id;
        copy_text(out[count].name, tasks[i].name, sizeof(out[count].name));
        out[count].state = tasks[i].state;
        out[count].foreground = tasks[i].foreground;
        out[count].cancel_requested = tasks[i].cancel_requested;
        out[count].wake_tick = tasks[i].wake_tick;
        out[count].blocked_event_id = tasks[i].blocked_event_id;
        out[count].priority = tasks[i].priority;
        out[count].parent_id = tasks[i].parent_id;
        out[count].exit_code = tasks[i].exit_code;
        count++;
    }

    return count;
}

int task_kill(int id) {
    return task_send_signal(id, LYTH_SIGTERM);
}

int task_send_signal(int id, int signum) {
    unsigned int flags;
    task_entry_t* task;
    int result;

    if (!signal_is_valid(signum)) {
        return 0;
    }

    flags = interrupt_save();
    task = find_task_by_id(id);
    if (task == 0) {
        interrupt_restore(flags);
        return 0;
    }

    result = task_queue_signal_locked(task, signum);
    interrupt_restore(flags);
    return result;
}

int task_set_signal_handler(int signum, unsigned int handler, unsigned int* old_handler_out) {
    unsigned int flags;
    task_entry_t* task;

    if (current_task_index < 0 || !signal_is_valid(signum) || signum == LYTH_SIGKILL) {
        return 0;
    }

    task = &tasks[current_task_index];

    if (!task->user_mode) {
        return 0;
    }

    flags = interrupt_save();
    if (old_handler_out != 0) {
        *old_handler_out = task->signal_handlers[signum];
    }
    task->signal_handlers[signum] = handler;
    interrupt_restore(flags);
    return 1;
}

unsigned int task_pending_signals(void) {
    if (current_task_index < 0) {
        return 0;
    }

    return tasks[current_task_index].pending_signals;
}

int task_sigprocmask(unsigned int how, unsigned int mask, unsigned int* old_mask_out) {
    unsigned int flags;
    task_entry_t* task;

    if (current_task_index < 0) {
        return 0;
    }

    task = &tasks[current_task_index];

    flags = interrupt_save();

    if (old_mask_out != 0) {
        *old_mask_out = task->signal_mask;
    }

    mask = signal_mask_sanitize(mask);
    switch (how) {
        case LYTH_SIG_BLOCK:
            task->signal_mask = signal_mask_sanitize(task->signal_mask | mask);
            break;
        case LYTH_SIG_UNBLOCK:
            task->signal_mask = signal_mask_sanitize(task->signal_mask & ~mask);
            break;
        case LYTH_SIG_SETMASK:
            task->signal_mask = signal_mask_sanitize(mask);
            break;
        default:
            interrupt_restore(flags);
            return 0;
    }

    interrupt_restore(flags);
    return 1;
}

int task_exec_current_user_from_frame(unsigned int frame_esp,
                                      const char* new_name,
                                      unsigned int entry_point,
                                      uint32_t user_physical_base,
                                      uint32_t user_heap_base,
                                      uint32_t user_heap_size,
                                      uint32_t* page_directory,
                                      uint32_t initial_user_esp) {
    unsigned int flags;
    task_entry_t* task;
    task_user_stack_frame_t* frame;
    uint32_t* old_page_directory;
    uint32_t old_user_physical_base;
    unsigned int top_user_esp;

    if (current_task_index < 0 || frame_esp == 0 || entry_point == 0 ||
        user_physical_base == 0 || page_directory == 0) {
        return 0;
    }

    task = &tasks[current_task_index];
    if (!task->used || !task->user_mode) {
        return 0;
    }

    top_user_esp = PAGING_USER_STACK_TOP - PAGING_USER_SIGNAL_TRAMPOLINE_SIZE;
    if (initial_user_esp == 0) {
        initial_user_esp = top_user_esp;
    }
    if (initial_user_esp < PAGING_USER_STACK_BOTTOM || initial_user_esp > top_user_esp) {
        return 0;
    }

    flags = interrupt_save();

    old_page_directory = task->page_directory;
    old_user_physical_base = task->user_physical_base;

    if (new_name != 0) {
        copy_text(task->name, new_name, TASK_NAME_MAX);
    }

    if (task->data != 0) {
        kfree(task->data);
        task->data = 0;
    }

    task->state = TASK_STATE_RUNNING;
    task->cancel_requested = 0;
    task->wake_tick = 0;
    task->blocked_event_id = -1;
    task->step = 0;
    task->data_size = 0;
    task->exit_code = 0;
    task->user_entry = entry_point;
    task->user_mode = 1;
    task->user_physical_base = user_physical_base;
    task->user_heap_base = user_heap_base;
    task->user_heap_size = user_heap_size;
    task->page_directory = page_directory;
    task->user_stack = (unsigned char*)(uintptr_t)PAGING_USER_STACK_BOTTOM;
    task->user_initial_esp = initial_user_esp;
    task->k_errno = 0;
    task->pending_signals = 0;
    task->alarm_tick = 0;           /* POSIX: pending alarm cancelled on exec */
    task->itimer_interval_ticks = 0;

    /* POSIX-like: caught handlers reset to default on exec; SIG_IGN preserved. */
    for (int signum = 1; signum <= LYTH_SIGNAL_MAX; signum++) {
        if (task->signal_handlers[signum] > LYTH_SIG_IGN) {
            task->signal_handlers[signum] = LYTH_SIG_DFL;
        }
    }

    task_install_signal_trampoline(task);

    frame = (task_user_stack_frame_t*)frame_esp;
    frame->eax = 0;
    frame->eip = entry_point;
    frame->cs = user_code_selector | 0x03;
    frame->eflags |= 0x00000200U;
    frame->user_esp = initial_user_esp;
    frame->user_ss = user_data_selector | 0x03;

    paging_switch_directory(page_directory);

    if (old_page_directory != 0) {
        shm_detach_all(old_page_directory, task->shm_mappings, SHM_MAX_MAPPINGS);
        task_reset_shm_mappings(task);
        paging_destroy_user_directory(old_page_directory);
    }
    if (old_user_physical_base != 0) {
        physmem_free_region(old_user_physical_base, PAGING_USER_SIZE);
    }

    interrupt_restore(flags);
    return 1;
}

void task_set_init_pid(int pid) {
    sys_init_pid = pid;
}

int task_reap_zombies_for(int parent_id) {
    unsigned int flags;
    int count = 0;
    int i;

    flags = interrupt_save();
    for (i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used &&
                tasks[i].state == TASK_STATE_ZOMBIE &&
                tasks[i].parent_id == parent_id) {
            clear_task(&tasks[i]);
            count++;
        }
    }
    interrupt_restore(flags);
    return count;
}

const char* task_state_name(task_state_t state) {
    switch (state) {
        case TASK_STATE_READY:
            return "READY";
        case TASK_STATE_RUNNING:
            return "RUNNING";
        case TASK_STATE_SLEEPING:
            return "SLEEP";
        case TASK_STATE_BLOCKED:
            return "BLOCK";
        case TASK_STATE_FINISHED:
            return "DONE";
        case TASK_STATE_ZOMBIE:
            return "ZOMBIE";
        case TASK_STATE_CANCELLED:
            return "CANCEL";
        default:
            return "FREE";
    }
}

unsigned int task_schedule_on_timer(unsigned int current_esp) {
    unsigned int flags = interrupt_save();
    unsigned int next_esp;

    if (current_task_index >= 0) {
        task_deliver_signals_current(&tasks[current_task_index], current_esp);
        next_esp = schedule_back_to_idle(current_esp);
    } else {
        next_esp = schedule_from_idle(current_esp);
    }

    interrupt_restore(flags);
    return next_esp;
}

/* ── UID/GID identity ────────────────────────────────────────────────────── */

unsigned int task_current_uid(void) {
    return (current_task_index >= 0) ? tasks[current_task_index].uid  : 0U;
}
unsigned int task_current_gid(void) {
    return (current_task_index >= 0) ? tasks[current_task_index].gid  : 0U;
}
unsigned int task_current_euid(void) {
    return (current_task_index >= 0) ? tasks[current_task_index].euid : 0U;
}
unsigned int task_current_egid(void) {
    return (current_task_index >= 0) ? tasks[current_task_index].egid : 0U;
}

static int task_has_group_locked(const task_entry_t* t, unsigned int gid) {
    int i;
    if (!t) return 0;
    if (t->egid == gid) return 1;
    for (i = 0; i < t->supp_group_count; i++) {
        if (t->supp_groups[i] == gid) return 1;
    }
    return 0;
}

/* setuid/setgid semantics (simplified POSIX):
 *  - Root (euid==0) can set uid to anything.
 *  - Non-root can only set euid back to real uid.
 *  Returns 0 on success, -1 on EPERM. */
int task_set_current_uid(unsigned int new_uid) {
    if (current_task_index < 0) return -1;
    if (tasks[current_task_index].euid == 0U) {
        /* root: set both real and effective */
        tasks[current_task_index].uid  = new_uid;
        tasks[current_task_index].euid = new_uid;
        return 0;
    }
    /* Non-root: only allowed to set euid to own real uid */
    if (new_uid == tasks[current_task_index].uid) {
        tasks[current_task_index].euid = new_uid;
        return 0;
    }
    return -1; /* EPERM */
}

int task_set_current_gid(unsigned int new_gid) {
    if (current_task_index < 0) return -1;
    if (tasks[current_task_index].euid == 0U) {
        tasks[current_task_index].gid  = new_gid;
        tasks[current_task_index].egid = new_gid;
        return 0;
    }
    if (new_gid == tasks[current_task_index].gid ||
        task_has_group_locked(&tasks[current_task_index], new_gid)) {
        tasks[current_task_index].egid = new_gid;
        return 0;
    }
    return -1;
}

/* Force-set identity (root-only, called from su/login path in kernel). */
void task_force_identity(unsigned int uid, unsigned int gid) {
    if (current_task_index < 0) return;
    tasks[current_task_index].uid  = uid;
    tasks[current_task_index].gid  = gid;
    tasks[current_task_index].euid = uid;
    tasks[current_task_index].egid = gid;
    tasks[current_task_index].supp_group_count = 0;
    for (int i = 0; i < TASK_MAX_SUPP_GROUPS; i++) tasks[current_task_index].supp_groups[i] = 0U;
}

int task_in_group(unsigned int gid) {
    if (current_task_index < 0) return 0;
    return task_has_group_locked(&tasks[current_task_index], gid);
}

int task_get_groups(unsigned int* gids_out, int max_groups) {
    int i;
    int n;
    if (current_task_index < 0) return -1;
    n = tasks[current_task_index].supp_group_count;
    if (!gids_out || max_groups <= 0) return n;
    if (max_groups < n) n = max_groups;
    for (i = 0; i < n; i++) gids_out[i] = tasks[current_task_index].supp_groups[i];
    return tasks[current_task_index].supp_group_count;
}

int task_set_groups(const unsigned int* gids, int count) {
    int i;
    if (current_task_index < 0) return -1;
    if (count < 0 || count > TASK_MAX_SUPP_GROUPS) return -1;
    if (tasks[current_task_index].euid != 0U) return -1; /* EPERM */

    tasks[current_task_index].supp_group_count = count;
    for (i = 0; i < TASK_MAX_SUPP_GROUPS; i++) {
        if (i < count) tasks[current_task_index].supp_groups[i] = gids ? gids[i] : 0U;
        else tasks[current_task_index].supp_groups[i] = 0U;
    }
    return 0;
}

int task_shm_create(unsigned int size) {
    unsigned int flags = interrupt_save();
    int rc = shm_create(size);
    interrupt_restore(flags);
    return rc;
}

uint32_t task_shm_attach(int segment_id) {
    unsigned int flags;
    uint32_t address;

    if (current_task_index < 0 || !tasks[current_task_index].used || !tasks[current_task_index].user_mode) {
        return 0U;
    }

    flags = interrupt_save();
    address = shm_attach(tasks[current_task_index].page_directory,
                         tasks[current_task_index].shm_mappings,
                         SHM_MAX_MAPPINGS,
                         segment_id);
    interrupt_restore(flags);
    return address;
}

int task_shm_detach(uint32_t address) {
    unsigned int flags;
    int rc;

    if (current_task_index < 0 || !tasks[current_task_index].used || !tasks[current_task_index].user_mode) {
        return -1;
    }

    flags = interrupt_save();
    rc = shm_detach(tasks[current_task_index].page_directory,
                    tasks[current_task_index].shm_mappings,
                    SHM_MAX_MAPPINGS,
                    address);
    interrupt_restore(flags);
    return rc ? 0 : -1;
}

int task_shm_unlink(int segment_id) {
    unsigned int flags = interrupt_save();
    int rc = shm_unlink(segment_id);
    interrupt_restore(flags);
    return rc;
}

int task_shm_list(shm_segment_info_t* out, int max_segments) {
    unsigned int flags = interrupt_save();
    int count = shm_list(out, max_segments);
    interrupt_restore(flags);
    return count;
}

int task_mq_create(unsigned int max_messages, unsigned int msg_size) {
    unsigned int flags = interrupt_save();
    int rc = mqueue_create(max_messages, msg_size);
    interrupt_restore(flags);
    return rc;
}

int task_mq_open(int queue_id, unsigned int open_flags) {
    unsigned int flags = interrupt_save();
    int rc = mqueue_open_fd(queue_id, open_flags);
    interrupt_restore(flags);
    return rc;
}

int task_mq_send(int queue_id, const void* message, unsigned int size) {
    unsigned int flags = interrupt_save();
    int rc = mqueue_send(queue_id, message, size);
    int read_event_id = (rc == 0) ? mqueue_read_event_id(queue_id) : -1;
    interrupt_restore(flags);

    if (rc == 0 && read_event_id >= 0) {
        task_signal_event(read_event_id);
    }

    return rc;
}

int task_mq_receive(int queue_id, void* buffer, unsigned int buffer_size, unsigned int* received_size_out) {
    unsigned int flags = interrupt_save();
    int rc = mqueue_receive(queue_id, buffer, buffer_size, received_size_out);
    int write_event_id = (rc == 0) ? mqueue_write_event_id(queue_id) : -1;
    interrupt_restore(flags);

    if (rc == 0 && write_event_id >= 0) {
        task_signal_event(write_event_id);
    }

    return rc;
}

int task_mq_send_timed(int queue_id, const void* message, unsigned int size, unsigned int timeout_ticks) {
    unsigned int waited = 0U;

    for (;;) {
        int rc = task_mq_send(queue_id, message, size);
        if (rc != MQ_E_FULL) {
            return rc;
        }

        if (timeout_ticks == 0U) {
            return MQ_E_TIMEOUT;
        }

        if (timeout_ticks != 0xFFFFFFFFU && waited >= timeout_ticks) {
            return MQ_E_TIMEOUT;
        }

        task_sleep(1);
        if (timeout_ticks != 0xFFFFFFFFU) {
            waited++;
        }
    }
}

int task_mq_receive_timed(int queue_id, void* buffer, unsigned int buffer_size, unsigned int timeout_ticks, unsigned int* received_size_out) {
    unsigned int waited = 0U;

    for (;;) {
        int rc = task_mq_receive(queue_id, buffer, buffer_size, received_size_out);
        if (rc != MQ_E_EMPTY) {
            return rc;
        }

        if (timeout_ticks == 0U) {
            return MQ_E_TIMEOUT;
        }

        if (timeout_ticks != 0xFFFFFFFFU && waited >= timeout_ticks) {
            return MQ_E_TIMEOUT;
        }

        task_sleep(1);
        if (timeout_ticks != 0xFFFFFFFFU) {
            waited++;
        }
    }
}

int task_mq_unlink(int queue_id) {
    unsigned int flags = interrupt_save();
    int read_event_id = mqueue_read_event_id(queue_id);
    int write_event_id = mqueue_write_event_id(queue_id);
    int rc = mqueue_unlink(queue_id);
    interrupt_restore(flags);

    if (rc == 0) {
        if (read_event_id >= 0) {
            task_signal_event(read_event_id);
        }
        if (write_event_id >= 0 && write_event_id != read_event_id) {
            task_signal_event(write_event_id);
        }
    }

    return rc;
}

int task_mq_list(mqueue_info_t* out, int max_queues) {
    unsigned int flags = interrupt_save();
    int count = mqueue_list(out, max_queues);
    interrupt_restore(flags);
    return count;
}

int task_mq_read_event_id(int queue_id) {
    unsigned int flags = interrupt_save();
    int event_id = mqueue_read_event_id(queue_id);
    interrupt_restore(flags);
    return event_id;
}

int task_mq_write_event_id(int queue_id) {
    unsigned int flags = interrupt_save();
    int event_id = mqueue_write_event_id(queue_id);
    interrupt_restore(flags);
    return event_id;
}

/* ── Resource-limit helpers ─────────────────────────────────────────────── */

int task_current_open_fd_count(void) {
    int i, count = 0;
    if (current_task_index < 0) return 0;
    for (i = 0; i < VFS_MAX_FD; i++) {
        if (tasks[current_task_index].fd_table[i].used)
            count++;
    }
    return count;
}

int task_get_fd_rlimit(unsigned int* soft_out, unsigned int* hard_out) {
    if (current_task_index < 0) return -1;
    if (soft_out) *soft_out = tasks[current_task_index].fd_limit_soft;
    if (hard_out) *hard_out = tasks[current_task_index].fd_limit_hard;
    return 0;
}

/* Rules (mirrors POSIX setrlimit semantics):
 *  - soft limit cannot exceed hard limit.
 *  - hard limit cannot exceed RLIM_NOFILE_HARD_MAX.
 *  - hard limit can only be lowered (not raised once set).
 *  Returns 0 on success, -1 on violation. */
int task_set_fd_rlimit(unsigned int new_soft, unsigned int new_hard) {
    if (current_task_index < 0) return -1;

    /* Hard limit is a one-way ratchet downwards */
    if (new_hard > tasks[current_task_index].fd_limit_hard) return -1;
    /* Hard limit ceiling */
    if (new_hard > RLIM_NOFILE_HARD_MAX) return -1;
    /* Soft must not exceed new hard */
    if (new_soft > new_hard) return -1;
    /* Soft must be at least 3 (keep stdio) */
    if (new_soft < 3U) return -1;

    tasks[current_task_index].fd_limit_soft = new_soft;
    tasks[current_task_index].fd_limit_hard = new_hard;
    return 0;
}

int task_fork_from_frame(unsigned int frame_esp) {
    task_user_stack_frame_t* parent_frame;
    task_entry_t* parent;
    uint32_t child_phys;
    uint32_t* child_dir;
    unsigned char* child_kstack;
    task_user_stack_frame_t* child_frame;
    unsigned int child_stack_top;
    int slot;
    unsigned int flags;
    uint32_t* src;
    uint32_t* dst;
    unsigned int words;

    if (current_task_index < 0) return -1;

    parent = &tasks[current_task_index];
    if (!parent->user_mode) return -1;

    /* The syscall_stub's pusha + CPU iret-frame sits at frame_esp,
       matching task_user_stack_frame_t exactly. */
    parent_frame = (task_user_stack_frame_t*)frame_esp;

    flags = interrupt_save();

    /* Find a free task slot */
    slot = -1;
    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (!tasks[i].used) { slot = i; break; }
    }
    if (slot < 0) { interrupt_restore(flags); return -1; }

    /* Allocate 4 MB physical region for child user space */
    child_phys = physmem_alloc_region(PAGING_USER_SIZE, PAGING_USER_SIZE);
    if (child_phys == 0) { interrupt_restore(flags); return -1; }

    /* Copy parent's user space (word-by-word for speed) */
    src   = (uint32_t*)(uintptr_t)parent->user_physical_base;
    dst   = (uint32_t*)(uintptr_t)child_phys;
    words = PAGING_USER_SIZE / sizeof(uint32_t);
    for (unsigned int i = 0; i < words; i++) dst[i] = src[i];

    /* Create child page directory */
    child_dir = paging_create_user_directory(child_phys);
    if (child_dir == 0) {
        physmem_free_region(child_phys, PAGING_USER_SIZE);
        interrupt_restore(flags);
        return -1;
    }

    /* Allocate child kernel stack */
    child_kstack = (unsigned char*)kmalloc(TASK_STACK_SIZE);
    if (child_kstack == 0) {
        paging_destroy_user_directory(child_dir);
        physmem_free_region(child_phys, PAGING_USER_SIZE);
        interrupt_restore(flags);
        return -1;
    }

    /* Fill child task entry */
    tasks[slot].used             = 1;
    tasks[slot].id               = next_task_id++;
    copy_text(tasks[slot].name, parent->name, TASK_NAME_MAX);
    tasks[slot].state            = TASK_STATE_READY;
    tasks[slot].foreground       = 0;   /* child runs in background */
    tasks[slot].cancel_requested = 0;
    tasks[slot].wake_tick        = 0;
    tasks[slot].blocked_event_id = -1;
    tasks[slot].priority         = parent->priority;
    tasks[slot].step             = 0;
    tasks[slot].data             = 0;
    tasks[slot].data_size        = 0;
    tasks[slot].exit_code        = 0;
    tasks[slot].output_vc        = parent->output_vc;
    tasks[slot].stack            = child_kstack;
    tasks[slot].user_stack       = parent->user_stack;
    tasks[slot].user_entry       = parent->user_entry;
    tasks[slot].user_mode        = 1;
    tasks[slot].user_physical_base = child_phys;
    tasks[slot].user_heap_base   = parent->user_heap_base;
    tasks[slot].user_heap_size   = parent->user_heap_size;
    tasks[slot].page_directory   = child_dir;
    tasks[slot].k_errno          = 0;
    tasks[slot].parent_id        = parent->id;   /* fork child's parent = the forking task */
    tasks[slot].sched_next       = -1;
    tasks[slot].fd_limit_soft = parent->fd_limit_soft;
    tasks[slot].fd_limit_hard = parent->fd_limit_hard;
    tasks[slot].uid  = parent->uid;
    tasks[slot].gid  = parent->gid;
    tasks[slot].euid = parent->euid;
    tasks[slot].egid = parent->egid;
    tasks[slot].supp_group_count = parent->supp_group_count;
    for (int gi = 0; gi < TASK_MAX_SUPP_GROUPS; gi++) {
        tasks[slot].supp_groups[gi] = parent->supp_groups[gi];
    }
    /* POSIX: alarm not inherited across fork — child starts with no pending alarm */
    tasks[slot].alarm_tick = 0;
    tasks[slot].itimer_interval_ticks = 0;
    task_signal_handlers_copy(&tasks[slot], parent);
    task_reset_shm_mappings(&tasks[slot]);
    if (!shm_clone_mappings(child_dir,
                            tasks[slot].shm_mappings,
                            SHM_MAX_MAPPINGS,
                            parent->shm_mappings,
                            SHM_MAX_MAPPINGS)) {
        kfree(child_kstack);
        paging_destroy_user_directory(child_dir);
        physmem_free_region(child_phys, PAGING_USER_SIZE);
        tasks[slot].used = 0;
        interrupt_restore(flags);
        return -1;
    }
    vfs_task_fd_inherit(tasks[slot].fd_table, parent->fd_table);  /* inherit open FDs */
    task_install_signal_trampoline(&tasks[slot]);

    /* Build child kernel stack: clone parent's syscall frame, child gets eax=0 */
    child_stack_top = (unsigned int)(child_kstack + TASK_STACK_SIZE);
    child_stack_top -= sizeof(task_user_stack_frame_t);
    child_frame = (task_user_stack_frame_t*)child_stack_top;
    *child_frame        = *parent_frame;
    child_frame->eax    = 0;   /* fork() returns 0 in the child */
    tasks[slot].saved_esp = child_stack_top;

    interrupt_restore(flags);
    sched_enqueue_ready(slot);
    return tasks[slot].id;   /* fork() returns child PID in the parent */
}

void task_set_errno(int e) {
    if (current_task_index >= 0)
        tasks[current_task_index].k_errno = e;
}

int task_get_errno(void) {
    if (current_task_index < 0) return 0;
    return tasks[current_task_index].k_errno;
}

void task_idle_stats(unsigned int* idle_ticks_out, unsigned int* ctx_switches_out) {
    if (idle_ticks_out != 0) {
        *idle_ticks_out = idle_tick_count;
    }
    if (ctx_switches_out != 0) {
        *ctx_switches_out = ctx_switch_count;
    }
}

int task_parent_id(int id) {
    unsigned int flags;
    task_entry_t* task;
    int result;

    flags = interrupt_save();
    task  = find_task_by_id(id);
    result = (task != 0) ? task->parent_id : -1;
    interrupt_restore(flags);
    return result;
}

/* Block the current task until the task with 'target_id' finishes.
 * The check and the blocking setup are done with interrupts disabled
 * to avoid a race where the target finishes between the existence check
 * and the wait_event call. */
void task_wait_id(int target_id) {
    unsigned int flags;
    task_entry_t* target;
    task_entry_t* self;
    int self_id;

    if (current_task_index < 0) return;

    self = &tasks[current_task_index];
    self_id = self->id;

    flags = interrupt_save();

    if (target_id == -1) {
        int has_child = 0;

        for (int i = 0; i < TASK_MAX_COUNT; i++) {
            if (!tasks[i].used || tasks[i].parent_id != self_id) {
                continue;
            }

            has_child = 1;
            if (tasks[i].state == TASK_STATE_ZOMBIE) {
                clear_task(&tasks[i]);
                interrupt_restore(flags);
                return;
            }
        }

        if (!has_child) {
            interrupt_restore(flags);
            return;
        }

        /* Wait for any child: complete_task() signals parent_id event. */
        self->blocked_event_id = self_id;
        self->state = TASK_STATE_BLOCKED;
        interrupt_restore(flags);
        return;
    }

    if (target_id < 0) {
        interrupt_restore(flags);
        return;
    }

    target = find_task_by_id(target_id);
    if (target == 0 || target->state == TASK_STATE_ZOMBIE) {
        /* If this is our zombie child, reap it now; else nothing to wait for. */
        if (target != 0 && target->state == TASK_STATE_ZOMBIE && target->parent_id == self_id) {
            clear_task(target);
        }
        interrupt_restore(flags);
        return;
    }

    /* Set up the block atomically while interrupts are off. */
    self->blocked_event_id = target_id;
    self->state = TASK_STATE_BLOCKED;
    interrupt_restore(flags);
}

/* ---- waitpid: wait with status collection ---- */
int task_waitpid(int target_id, uint32_t status_uptr) {
    unsigned int flags;
    task_entry_t* self;
    task_entry_t* target;
    int self_id;

    if (current_task_index < 0) return -1;

    self = &tasks[current_task_index];
    self_id = self->id;

    flags = interrupt_save();

    if (target_id == -1) {
        /* Any child */
        int has_child = 0;

        for (int i = 0; i < TASK_MAX_COUNT; i++) {
            if (!tasks[i].used || tasks[i].parent_id != self_id) continue;
            has_child = 1;
            if (tasks[i].state == TASK_STATE_ZOMBIE) {
                int child_pid  = tasks[i].id;
                int child_stat = encode_child_status(&tasks[i]);
                clear_task(&tasks[i]);
                interrupt_restore(flags);
                if (status_uptr)
                    write_user_int32(self, status_uptr, child_stat);
                return child_pid;
            }
        }

        if (!has_child) {
            interrupt_restore(flags);
            task_set_errno(10); /* ECHILD */
            return -1;
        }

        /* Block; complete_task() will fill frame->eax and write status. */
        self->waitpid_status_uptr = status_uptr;
        self->blocked_event_id = self_id;
        self->state = TASK_STATE_BLOCKED;
        interrupt_restore(flags);
        return 0; /* overridden by complete_task */
    }

    if (target_id <= 0) {
        interrupt_restore(flags);
        task_set_errno(22); /* EINVAL */
        return -1;
    }

    target = find_task_by_id(target_id);

    if (target == 0) {
        interrupt_restore(flags);
        task_set_errno(10); /* ECHILD */
        return -1;
    }

    if (target->parent_id != self_id) {
        interrupt_restore(flags);
        task_set_errno(10); /* ECHILD */
        return -1;
    }

    if (target->state == TASK_STATE_ZOMBIE) {
        int child_pid  = target->id;
        int child_stat = encode_child_status(target);
        clear_task(target);
        interrupt_restore(flags);
        if (status_uptr)
            write_user_int32(self, status_uptr, child_stat);
        return child_pid;
    }

    /* Block waiting for this specific child. */
    self->waitpid_status_uptr = status_uptr;
    self->blocked_event_id = target_id;
    self->state = TASK_STATE_BLOCKED;
    interrupt_restore(flags);
    return 0; /* overridden by complete_task */
}

unsigned int task_schedule_on_syscall(unsigned int current_esp) {
    unsigned int flags = interrupt_save();
    unsigned int next_esp = current_esp;

    if (current_task_index >= 0) {
        task_entry_t* task = &tasks[current_task_index];

        task_deliver_signals_current(task, current_esp);

        if (task->state != TASK_STATE_RUNNING) {
            next_esp = schedule_back_to_idle(current_esp);
        }
    }

    interrupt_restore(flags);
    return next_esp;
}
