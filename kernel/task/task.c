#include "task.h"
#include "heap.h"
#include "timer.h"
#include "syscall.h"
#include "gdt.h"
#include "paging.h"
#include "physmem.h"
#include <stdint.h>

#define TASK_MAX_COUNT 8
#define TASK_NAME_MAX 24
#define TASK_STACK_SIZE 4096
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
    uint32_t* page_directory;
} task_entry_t;

static task_entry_t tasks[TASK_MAX_COUNT];
static int current_task_index = -1;
static int next_task_id = 1;
static int scheduler_cursor = 0;
static int foreground_task_id = -1;
static unsigned int idle_context_esp = 0;
static unsigned short kernel_code_selector = 0x08;
static unsigned short user_code_selector = GDT_USER_CODE_SELECTOR;
static unsigned short user_data_selector = GDT_USER_DATA_SELECTOR;
static void (*foreground_complete_handler)(int id, const char* name, int cancelled) = 0;

static void task_thread_bootstrap(void);

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

    if (task->stack != 0) {
        kfree(task->stack);
    }

    if (task->user_stack != 0 && task->user_mode == 0) {
        kfree(task->user_stack);
    }

    if (task->page_directory != 0) {
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
    task->page_directory = 0;
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

static int select_next_ready_task(void) {
    for (int priority = TASK_PRIORITY_HIGH; priority <= TASK_PRIORITY_LOW; priority++) {
        for (int offset = 0; offset < TASK_MAX_COUNT; offset++) {
            int index = (scheduler_cursor + offset) % TASK_MAX_COUNT;

            if (tasks[index].used &&
                tasks[index].state == TASK_STATE_READY &&
                tasks[index].priority == priority) {
                scheduler_cursor = (index + 1) % TASK_MAX_COUNT;
                return index;
            }
        }
    }

    return -1;
}

static unsigned int schedule_from_idle(unsigned int current_esp) {
    int selected = select_next_ready_task();

    if (selected < 0) {
        return current_esp;
    }

    idle_context_esp = current_esp;
    current_task_index = selected;
    tasks[selected].state = TASK_STATE_RUNNING;

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

    paging_switch_directory(paging_kernel_directory());

    if (task->state == TASK_STATE_RUNNING) {
        task->state = TASK_STATE_READY;
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
    }

    current_task_index = -1;
    next_task_id = 1;
    scheduler_cursor = 0;
    foreground_task_id = -1;
    idle_context_esp = 0;
    foreground_complete_handler = 0;

    __asm__ volatile ("mov %%cs, %0" : "=r"(kernel_code_selector));
    user_code_selector = gdt_user_code_selector();
    user_data_selector = gdt_user_data_selector();
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
    tasks[slot].saved_esp = 0;
    tasks[slot].stack = 0;
    tasks[slot].user_stack = 0;
    tasks[slot].user_entry = 0;
    tasks[slot].user_mode = 0;

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

    interrupt_restore(flags);
    return tasks[slot].id;
}

int task_spawn_user(const char* name,
                    unsigned int entry_point,
                    uint32_t user_physical_base,
                    uint32_t user_heap_base,
                    uint32_t user_heap_size,
                    uint32_t* page_directory,
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
    tasks[slot].saved_esp = 0;
    tasks[slot].stack = (unsigned char*)kmalloc(TASK_STACK_SIZE);
    tasks[slot].user_stack = 0;
    tasks[slot].user_entry = entry_point;
    tasks[slot].user_mode = 1;
    tasks[slot].user_physical_base = user_physical_base;
    tasks[slot].user_heap_base = user_heap_base;
    tasks[slot].user_heap_size = user_heap_size;
    tasks[slot].page_directory = page_directory;

    if (tasks[slot].stack == 0) {
        clear_task(&tasks[slot]);
        interrupt_restore(flags);
        return -1;
    }

    tasks[slot].user_stack = (unsigned char*)(uintptr_t)(PAGING_USER_BASE + PAGING_USER_SIZE - TASK_USER_STACK_SIZE);

    initialize_task_stack(&tasks[slot]);

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
    (void)0;
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
    tasks[current_task_index].blocked_event_id = -1;
    tasks[current_task_index].state = TASK_STATE_SLEEPING;
}

void task_wait_event(int event_id) {
    if (current_task_index < 0 || event_id < 0) {
        return;
    }

    tasks[current_task_index].blocked_event_id = event_id;
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
            tasks[i].state = TASK_STATE_READY;
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
    if (task->state == TASK_STATE_SLEEPING || task->state == TASK_STATE_BLOCKED) {
        task->blocked_event_id = -1;
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

    if (priority < TASK_PRIORITY_HIGH || priority > TASK_PRIORITY_LOW) {
        return 0;
    }

    flags = interrupt_save();
    task = find_task_by_id(id);

    if (task == 0) {
        interrupt_restore(flags);
        return 0;
    }

    task->priority = priority;
    interrupt_restore(flags);
    return 1;
}

const char* task_priority_name(task_priority_t priority) {
    switch (priority) {
        case TASK_PRIORITY_HIGH:
            return "HIGH";
        case TASK_PRIORITY_LOW:
            return "LOW";
        default:
            return "NORMAL";
    }
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
        out[count].blocked_event_id = tasks[i].blocked_event_id;
        out[count].priority = tasks[i].priority;
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
        case TASK_STATE_BLOCKED:
            return "BLOCK";
        case TASK_STATE_FINISHED:
            return "DONE";
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
        next_esp = schedule_back_to_idle(current_esp);
    } else {
        next_esp = schedule_from_idle(current_esp);
    }

    interrupt_restore(flags);
    return next_esp;
}

unsigned int task_schedule_on_syscall(unsigned int current_esp) {
    unsigned int flags = interrupt_save();
    unsigned int next_esp = current_esp;

    if (current_task_index >= 0) {
        task_entry_t* task = &tasks[current_task_index];

        if (task->state != TASK_STATE_RUNNING) {
            next_esp = schedule_back_to_idle(current_esp);
        }
    }

    interrupt_restore(flags);
    return next_esp;
}
