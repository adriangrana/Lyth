#ifndef TASK_H
#define TASK_H

typedef enum {
	TASK_STATE_FREE = 0,
	TASK_STATE_READY,
	TASK_STATE_RUNNING,
	TASK_STATE_SLEEPING,
	TASK_STATE_FINISHED,
	TASK_STATE_CANCELLED
} task_state_t;

typedef void (*task_step_fn)(void);

typedef struct {
	int id;
	char name[24];
	task_state_t state;
	int foreground;
	int cancel_requested;
	unsigned int wake_tick;
} task_snapshot_t;

void task_system_init(void);
int task_spawn(const char* name, task_step_fn step, const void* data, unsigned int data_size, int foreground);
void task_run_ready(void);
int task_has_runnable(void);
void task_on_tick(void);
void task_sleep(unsigned int ticks);
void task_yield(void);
void task_exit(int exit_code);
void task_request_cancel(void);
int task_cancel_requested(void);
void task_clear_cancel(void);
int task_is_running(void);
const char* task_current_name(void);
int task_current_id(void);
void* task_current_data(void);
int task_has_foreground_task(void);
int task_foreground_task_id(void);
void task_set_foreground_complete_handler(void (*handler)(int id, const char* name, int cancelled));
int task_list(task_snapshot_t* out, int max_tasks);
int task_kill(int id);
const char* task_state_name(task_state_t state);

#endif
