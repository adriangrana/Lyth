#ifndef TASK_H
#define TASK_H

#include <stdint.h>

typedef enum {
	TASK_STATE_FREE = 0,
	TASK_STATE_READY,
	TASK_STATE_RUNNING,
	TASK_STATE_SLEEPING,
	TASK_STATE_BLOCKED,
	TASK_STATE_FINISHED,
	TASK_STATE_CANCELLED
} task_state_t;

typedef enum {
	TASK_PRIORITY_HIGH = 0,
	TASK_PRIORITY_NORMAL = 1,
	TASK_PRIORITY_LOW = 2
} task_priority_t;

typedef void (*task_step_fn)(void);

typedef struct {
	int id;
	char name[24];
	task_state_t state;
	int foreground;
	int cancel_requested;
	unsigned int wake_tick;
	int blocked_event_id;
	task_priority_t priority;
} task_snapshot_t;

void task_system_init(void);
int task_spawn(const char* name, task_step_fn step, const void* data, unsigned int data_size, int foreground);
int task_spawn_user(const char* name,
					unsigned int entry_point,
					uint32_t user_physical_base,
					uint32_t user_heap_base,
					uint32_t user_heap_size,
					uint32_t* page_directory,
					int foreground);
void task_run_ready(void);
int task_has_runnable(void);
void task_on_tick(void);
unsigned int task_schedule_on_timer(unsigned int current_esp);
unsigned int task_schedule_on_syscall(unsigned int current_esp);
void task_sleep(unsigned int ticks);
void task_wait_event(int event_id);
int task_signal_event(int event_id);
void task_yield(void);
void task_exit(int exit_code);
void task_request_cancel(void);
int task_cancel_requested(void);
void task_clear_cancel(void);
int task_is_running(void);
int task_current_is_user_mode(void);
uint32_t task_current_user_heap_base(void);
uint32_t task_current_user_heap_size(void);
const char* task_current_name(void);
int task_current_id(void);
void* task_current_data(void);
int task_has_foreground_task(void);
int task_foreground_task_id(void);
int task_set_priority(int id, task_priority_t priority);
task_priority_t task_current_priority(void);
const char* task_priority_name(task_priority_t priority);
void task_set_foreground_complete_handler(void (*handler)(int id, const char* name, int cancelled));
int task_list(task_snapshot_t* out, int max_tasks);
int task_kill(int id);
const char* task_state_name(task_state_t state);

#endif
