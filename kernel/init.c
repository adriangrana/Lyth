/* ============================================================
 *  init.c  —  PID-1 "init" task
 *
 *  Responsibilities:
 *    - First scheduled task in the system (guaranteed PID 1).
 *    - Manages the boot state machine:
 *        splash → login → session (desktop) → [logout → login]
 *    - In recovery / debug mode, drops to the interactive shell.
 *    - Reaps zombie children reparented to init.
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
#include "splash.h"
#include "login.h"
#include "session.h"
#include "dhcp.h"
#include "netif.h"
#include "e1000.h"
#include "wifi.h"
#include "timer.h"
#include "klog.h"

static int init_task_pid   = -1;
static boot_state_t  current_boot_state = BOOT_STATE_KERNEL;
static boot_mode_t   current_boot_mode  = BOOT_MODE_NORMAL;

/* ---- recovery / debug shell (same as old init_step) ---------- */

static int recovery_shell_initialized = 0;

static void recovery_shell_step(void) {
    input_event_t   event;
    mouse_state_t   mouse_state;
    int             got_event = 0;

    if (!recovery_shell_initialized) {
        terminal_print_line("Lyth OS recovery shell");
        terminal_print_line("Type 'gui' to start desktop, 'reboot' to restart.");
        shell_input_init();
        recovery_shell_initialized = 1;
    }

    if (init_task_pid > 0)
        task_reap_zombies_for(init_task_pid);

    usb_hid_poll();

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
    if (!got_event)
        task_yield();
}

/* ---- normal boot flow --------------------------------------- */

static void normal_boot_step(void) {
    unsigned int uid;

    if (init_task_pid > 0)
        task_reap_zombies_for(init_task_pid);

    switch (current_boot_state) {
    case BOOT_STATE_KERNEL:
    case BOOT_STATE_SERVICES:
        /* Splash is already displayed by kernel_main.
         * Just yield until kernel_main advances the state. */
        task_yield();
        break;

    case BOOT_STATE_GRAPHICS:
        /* Splash is done, initialise GUI subsystem. */
        splash_hide();
        gui_init();
        klog_write(KLOG_LEVEL_INFO, "init", "GUI subsystem ready");
        current_boot_state = BOOT_STATE_LOGIN;
        break;

    case BOOT_STATE_LOGIN:
        klog_write(KLOG_LEVEL_INFO, "init", "Starting login manager");

        /* login_manager_run blocks until the user authenticates */
        uid = login_manager_run();

        if (uid == (unsigned int)-1) {
            /* Login failed or couldn't run — fall back to recovery */
            klog_write(KLOG_LEVEL_WARN, "init", "Login manager failed, entering recovery");
            current_boot_state = BOOT_STATE_RECOVERY;
            break;
        }

        /* Create user session */
        if (session_create(uid) < 0) {
            klog_write(KLOG_LEVEL_WARN, "init", "Session creation failed");
            current_boot_state = BOOT_STATE_RECOVERY;
            break;
        }

        klog_write(KLOG_LEVEL_INFO, "init", "User session started");

        /* Auto-configure network via DHCP (only if link is up) */
        {
            netif_t* iface = netif_get_default();
            if (iface && iface->ip_addr == 0 && e1000_link_up()) {
                klog_write(KLOG_LEVEL_INFO, "init", "Link up, starting DHCP...");
                dhcp_discover(iface);
                {
                    unsigned int deadline = timer_get_uptime_ms() + 3000;
                    while (!dhcp_get_result()->ok &&
                           timer_get_uptime_ms() < deadline) {
                        e1000_poll_rx();
                        task_yield();
                    }
                }
                if (dhcp_get_result()->ok)
                    klog_write(KLOG_LEVEL_INFO, "init", "DHCP OK");
                else
                    klog_write(KLOG_LEVEL_WARN, "init", "DHCP timeout");
            } else if (iface && iface->ip_addr == 0) {
                klog_write(KLOG_LEVEL_INFO, "init", "No link, skipping DHCP");
            }
        }

        /* Initialise WiFi subsystem (virtual adapter over eth0) */
        if (wifi_init() == 0)
            klog_write(KLOG_LEVEL_INFO, "init", "WiFi adapter ready");

        current_boot_state = BOOT_STATE_SESSION;
        break;

    case BOOT_STATE_SESSION:
        /* Run the desktop compositor (blocks until logout/exit) */
        gui_run();

        /* Desktop exited — destroy session and return to login */
        session_destroy();
        klog_write(KLOG_LEVEL_INFO, "init", "Session ended, returning to login");

        /* Re-initialise GUI for next login cycle */
        gui_init();
        current_boot_state = BOOT_STATE_LOGIN;
        break;

    case BOOT_STATE_RECOVERY:
        recovery_shell_step();
        break;
    }
}

/* ---- init task step ----------------------------------------- */

static void init_step(void) {
    if (current_boot_mode == BOOT_MODE_RECOVERY ||
        current_boot_mode == BOOT_MODE_DEBUG) {
        recovery_shell_step();
    } else {
        normal_boot_step();
    }
}

/* ---- public API --------------------------------------------- */

void init_start(void) {
    session_init();

    init_task_pid = task_spawn("init", init_step, 0, 0, 0);
    if (init_task_pid > 0) {
        task_set_priority(init_task_pid, TASK_PRIORITY_NORMAL);
        task_set_output_vc(init_task_pid, -1);
        task_set_init_pid(init_task_pid);
    }
}

int init_pid(void) {
    return init_task_pid > 0 ? init_task_pid : 1;
}

boot_state_t init_get_boot_state(void) {
    return current_boot_state;
}

boot_mode_t init_get_boot_mode(void) {
    return current_boot_mode;
}

void init_set_boot_mode(boot_mode_t mode) {
    current_boot_mode = mode;
}

void init_set_boot_state(boot_state_t state) {
    current_boot_state = state;
}
