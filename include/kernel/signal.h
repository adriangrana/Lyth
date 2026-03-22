#ifndef KERNEL_SIGNAL_H
#define KERNEL_SIGNAL_H

/* Basic signal set used by the kernel/userland ABI. */
enum {
    LYTH_SIGINT  = 2,
    LYTH_SIGKILL = 9,
    LYTH_SIGUSR1 = 10,
    LYTH_SIGUSR2 = 12,
    LYTH_SIGALRM = 14,
    LYTH_SIGTERM = 15,
    LYTH_SIGCHLD = 17,
    LYTH_SIGNAL_MAX = 31
};

/* Signal disposition values stored by the kernel. */
#define LYTH_SIG_DFL 0U
#define LYTH_SIG_IGN 1U

/* sigprocmask-like operations */
#define LYTH_SIG_BLOCK   0U
#define LYTH_SIG_UNBLOCK 1U
#define LYTH_SIG_SETMASK 2U

#endif