#include "task.h"
#include "heap.h"
#include "timer.h"

#define TASK_MAX_COUNT 8
#define TASK_NAME_MAX 24

typedef struct {
    int used;
    int id;
    char name[TASK_NAME_MAX];
    task_state_t state;
    int foreground;
    int cancel_requested;
    unsigned int wake_tick;
    task_step_fn step;
    void* data;
    unsigned int data_size;
    int exit_code;
} task_entry_t;

static task_entry_t tasks[TASK_MAX_COUNT];
static int current_task_index = -1;
static int next_task_id = 1;
static int scheduler_cursor = 0;
static int foreground_task_id = -1;
static void (*foreground_complete_handler)(int id, const char* name, int cancelled) = 0;

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

    if (task->data != 0) {
        kfree(task->data);
    }

    task->used = 0;
    task->id = 0;
    task->name[0] = '\0';
    task->state = TASK_STATE_FREE;
    task->foreground = 0;
    task->cancel_requested = 0;
    task->wake_tick = 0;
    task->step = 0;
    task->data = 0;
    task->data_size = 0;
    task->exit_code = 0;
}

static void complete_task(task_entry_t* task) {
    int task_id;
    const char* task_name;
    int cancelled;
    int notify_foreground;

    if (task == 0 || !task->used) {
        return;
    }

    task_id = task->id;
    task_name = task->name;
    cancelled = task->cancel_requested || task->state == TASK_STATE_CANCELLED;
    notify_foreground = task->foreground && foreground_task_id == task->id;

    if (notify_foreground) {
        foreground_task_id = -1;
    }

    if (notify_foreground && foreground_complete_handler != 0) {
        foreground_complete_handler(task_id, task_name, cancelled);
    }

    clear_task(task);
}

void task_system_init(void) {
    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        tasks[i].used = 0;
        tasks[i].data = 0;
    }

    current_task_index = -1;
    next_task_id = 1;
    scheduler_cursor = 0;
    foreground_task_id = -1;
    foreground_complete_handler = 0;
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
    tasks[slot].step = step;
    tasks[slot].data = 0;
    tasks[slot].data_size = data_size;
    tasks[slot].exit_code = 0;

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

    if (foreground) {
        foreground_task_id = tasks[slot].id;
    }

    interrupt_restore(flags);
    return tasks[slot].id;
}

void task_on_tick(void) {
    unsigned int now = timer_get_ticks();

    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].state == TASK_STATE_SLEEPING) {
            if ((int)(now - tasks[i].wake_tick) >= 0) {
                tasks[i].state = TASK_STATE_READY;
            }
        }
    }
}

void task_run_ready(void) {
    unsigned int flags;
    int selected = -1;

    flags = interrupt_save();

    for (int offset = 0; offset < TASK_MAX_COUNT; offset++) {
        int index = (scheduler_cursor + offset) % TASK_MAX_COUNT;

        if (tasks[index].used && tasks[index].state == TASK_STATE_READY) {
            selected = index;
            scheduler_cursor = (index + 1) % TASK_MAX_COUNT;
            tasks[index].state = TASK_STATE_RUNNING;
            current_task_index = index;
            break;
        }
    }

    interrupt_restore(flags);

    if (selected < 0) {
        return;
    }

    tasks[selected].step();

    flags = interrupt_save();

    if (current_task_index == selected) {
        if (tasks[selected].used && tasks[selected].state == TASK_STATE_RUNNING) {
            tasks[selected].state = TASK_STATE_READY;
        }

        current_task_index = -1;
    }

    if (tasks[selected].used &&
        (tasks[selected].state == TASK_STATE_FINISHED || tasks[selected].state == TASK_STATE_CANCELLED)) {
        complete_task(&tasks[selected]);
    }

    interrupt_restore(flags);
}

int task_has_runnable(void) {
    for (int i = 0; i < TASK_MAX_COUNT; i++) {
        if (tasks[i].used && tasks[i].state == TASK_STATE_READY) {
            return 1;
        }
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
    tasks[current_task_index].state = TASK_STATE_SLEEPING;
}

void task_yield(void) {
    if (current_task_index < 0) {
        return;
    }

    tasks[current_task_index].state = TASK_STATE_READY;
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
    } else if (foreground_task_id != -1) {
        task = find_task_by_id(foreground_task_id);
    }

    if (task == 0) {
        return;
    }

    task->cancel_requested = 1;
    if (task->state == TASK_STATE_SLEEPING) {
        task->state = TASK_STATE_READY;
    }
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

void task_set_foreground_complete_handler(void (*handler)(int id, const char* name, int cancelled)) {
    foreground_complete_handler = handler;
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
        count++;
    }

    return count;
}

int task_kill(int id) {
    unsigned int flags;
    task_entry_t* task;

    flags = interrupt_save();
    task = find_task_by_id(id);

    if (task == 0) {
        interrupt_restore(flags);
        return 0;
    }

    if (task->state == TASK_STATE_RUNNING) {
        task->cancel_requested = 1;
        interrupt_restore(flags);
        return 1;
    }

    task->cancel_requested = 1;
    task->state = TASK_STATE_CANCELLED;
    complete_task(task);
    interrupt_restore(flags);
    return 1;
}

const char* task_state_name(task_state_t state) {
    switch (state) {
        case TASK_STATE_READY:
            return "READY";
        case TASK_STATE_RUNNING:
            return "RUNNING";
        case TASK_STATE_SLEEPING:
            return "SLEEP";
        case TASK_STATE_FINISHED:
            return "DONE";
        case TASK_STATE_CANCELLED:
            return "CANCEL";
        default:
            return "FREE";
    }
}
