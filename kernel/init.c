/* ============================================================
 *  init.c  —  PID-1 "init" task
 *
 *  Responsibilities:
 *    - First scheduled task in the system (guaranteed PID 1).
 *    - Owns the keyboard/mouse event loop and the interactive shell.
 *    - Runs at TASK_PRIORITY_HIGH so the shell stays responsive.
 *    - Yields when there are no pending events (avoids spinning).
 *
 *  All other tasks (userland ELFs, background jobs) are children or
 *  grandchildren of this task via the parent_id chain.
 * ============================================================ */

#include "init.h"
#include "task.h"
#include "input.h"
#include "mouse.h"
#include "fbconsole.h"
#include "terminal.h"
#include "shell_input.h"
#include "compositor.h"
#include "usb_hid.h"

static int init_task_pid = -1;

/* ---- init task step ----------------------------------------- */

static void init_step(void) {
    input_event_t   event;
    mouse_state_t   mouse_state;
    int             got_event = 0;
    static int      initialized = 0;

    /* One-time shell initialisation on first scheduling. */
    if (!initialized) {
        shell_input_init();
        initialized = 1;
    }

    /* Reap any zombie children that have been reparented to (or spawned by) init. */
    if (init_task_pid > 0) {
        task_reap_zombies_for(init_task_pid);
    }

    /* Poll USB HID devices for input before draining the queue. */
    usb_hid_poll();

    /* Drain the input queue. */
    while (input_poll_event(&event)) {
        got_event = 1;

        if (event.device_type == INPUT_DEVICE_MOUSE && fb_active()
            && !gui_is_active()) {
            mouse_get_state(&mouse_state);
            fb_move_mouse_cursor(mouse_state.x, mouse_state.y);
        }

        if (!gui_is_active())
            shell_input_handle_event(&event);
    }

    terminal_update_cursor();

    /* Nothing to do — yield so lower-priority tasks can run. */
    if (!got_event) {
        task_yield();
    }
}

/* ---- public API --------------------------------------------- */

void init_start(void) {
    init_task_pid = task_spawn("init", init_step, 0, 0, 0);
    if (init_task_pid > 0) {
        task_set_priority(init_task_pid, TASK_PRIORITY_NORMAL);
        task_set_output_vc(init_task_pid, -1);  /* init follows active VC */
        task_set_init_pid(init_task_pid);  /* let the task subsystem know who init is */
    }
}

int init_pid(void) {
    return init_task_pid > 0 ? init_task_pid : 1;
}
