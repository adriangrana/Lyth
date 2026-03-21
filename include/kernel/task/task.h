#ifndef TASK_H
#define TASK_H

#include <stdint.h>
#include "vfs.h"
#include "signal.h"

typedef enum {
	TASK_STATE_FREE = 0,
	TASK_STATE_READY,
	TASK_STATE_RUNNING,
	TASK_STATE_SLEEPING,
	TASK_STATE_BLOCKED,
	TASK_STATE_FINISHED,
	TASK_STATE_ZOMBIE,		/* exited, waiting for parent to collect */
	TASK_STATE_CANCELLED
} task_state_t;

typedef enum {
	TASK_PRIORITY_HIGH = 0,
	TASK_PRIORITY_NORMAL = 1,
	TASK_PRIORITY_LOW = 2,
	TASK_PRIORITY_IDLE = 3		/* reserved for the system idle task only */
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
	int parent_id;
	int exit_code;			/* exit status; meaningful when state == TASK_STATE_ZOMBIE */
} task_snapshot_t;

void task_system_init(void);
int task_spawn(const char* name, task_step_fn step, const void* data, unsigned int data_size, int foreground);
int task_spawn_user(const char* name,
					unsigned int entry_point,
					uint32_t user_physical_base,
					uint32_t user_heap_base,
					uint32_t user_heap_size,
					uint32_t* page_directory,
					uint32_t initial_user_esp,
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
void task_set_errno(int e);
int task_get_errno(void);
void task_wait_id(int target_id);
/* Wait for a child to exit and collect its POSIX exit status.
   target_id: child PID to wait for, or -1 for any child.
   status_uptr: virtual address where the int status will be written (0 = discard).
   Returns child PID on success, -1 if no matching child or error. */
int task_waitpid(int target_id, uint32_t status_uptr);
/* Returns the parent PID of the given task, or -1 if not found. */
int task_parent_id(int id);
/* Send a signal to a task by PID. Returns 1 on success, 0 on error. */
int task_send_signal(int id, int signum);
/* Set current task signal disposition for signum.
	handler uses LYTH_SIG_DFL / LYTH_SIG_IGN or a user function address.
	old_handler_out may be NULL. Returns 1 on success. */
int task_set_signal_handler(int signum, unsigned int handler, unsigned int* old_handler_out);
/* Pending signals bitmask for current task. */
unsigned int task_pending_signals(void);
/* Update current task signal mask. how=LYTH_SIG_BLOCK/UNBLOCK/SETMASK. */
int task_sigprocmask(unsigned int how, unsigned int mask, unsigned int* old_mask_out);
/* Replace current user task image in-place (exec semantics). */
int task_exec_current_user_from_frame(unsigned int frame_esp,
									  const char* new_name,
									  unsigned int entry_point,
									  uint32_t user_physical_base,
									  uint32_t user_heap_base,
									  uint32_t user_heap_size,
									  uint32_t* page_directory,
									  uint32_t initial_user_esp);
/* Fork the current user-mode task from a syscall interrupt frame.
   Returns the child task ID in the parent, or -1 on error.
   Must only be called from syscall_interrupt_handler. */
int task_fork_from_frame(unsigned int frame_esp);
/* Inform the task subsystem of the init PID; called once from init_start(). */
void task_set_init_pid(int pid);
/* Non-blocking: reap all zombie children of parent_id. Returns count reaped. */
int  task_reap_zombies_for(int parent_id);
/* Returns a pointer to the current task's fd table, or NULL if no task runs. */
vfs_fd_entry_t* task_current_fd_table(void);
/* Returns a pointer to the fd table of the task with the given ID, or NULL. */
vfs_fd_entry_t* task_get_fd_table(int task_id);
/* Idle task statistics: ticks spent idle and total context switches. */
void task_idle_stats(unsigned int* idle_ticks_out, unsigned int* ctx_switches_out);

/* ---- Resource limits (RLIMIT_NOFILE) ---- */
/* Count how many FD slots are currently used by the running task. */
int task_current_open_fd_count(void);
/* Read the soft and hard NOFILE limits of the running task. */
int task_get_fd_rlimit(unsigned int* soft_out, unsigned int* hard_out);
/* Update limits. soft <= hard <= RLIM_NOFILE_HARD_MAX; hard is a downward ratchet.
   Returns 0 on success, -1 on violation. */
int task_set_fd_rlimit(unsigned int new_soft, unsigned int new_hard);

#endif
