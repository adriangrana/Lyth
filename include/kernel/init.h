#ifndef INIT_H
#define INIT_H

/* ---- Boot states ---- */
typedef enum {
    BOOT_STATE_KERNEL,     /* Core hardware init (GDT, IDT, memory, scheduler) */
    BOOT_STATE_SERVICES,   /* Device detection (PCI, disk, net, USB) */
    BOOT_STATE_GRAPHICS,   /* GUI subsystem initialisation */
    BOOT_STATE_LOGIN,      /* Login manager waiting for credentials */
    BOOT_STATE_SESSION,    /* User desktop session active */
    BOOT_STATE_RECOVERY    /* Recovery shell (fallback / debug) */
} boot_state_t;

/* ---- Boot mode (selected at early boot) ---- */
typedef enum {
    BOOT_MODE_NORMAL,      /* splash → login → desktop */
    BOOT_MODE_RECOVERY,    /* direct to recovery shell */
    BOOT_MODE_DEBUG        /* verbose boot + shell, no GUI auto-start */
} boot_mode_t;

/* Spawn the init task (PID 1).  Call after task_system_init(). */
void init_start(void);

/* Returns the fixed PID of the init task (always 1). */
int  init_pid(void);

/* Boot state / mode queries */
boot_state_t init_get_boot_state(void);
boot_mode_t  init_get_boot_mode(void);
void         init_set_boot_mode(boot_mode_t mode);

/* Advance the boot state (called by kernel_main during init sequence). */
void init_set_boot_state(boot_state_t state);

#endif /* INIT_H */
