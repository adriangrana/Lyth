#ifndef INIT_H
#define INIT_H

/* Spawn the init task (PID 1) and register it as the high-priority
 * event/shell loop.  Must be called after task_system_init() and before
 * interrupts_init() so that the init task is guaranteed to receive PID 1. */
void init_start(void);

/* Returns the fixed PID of the init task (always 1). */
int  init_pid(void);

#endif
