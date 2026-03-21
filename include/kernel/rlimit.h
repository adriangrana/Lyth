#ifndef RLIMIT_H
#define RLIMIT_H

/* ---- POSIX-compatible resource IDs ---- */
#define RLIMIT_NOFILE   7U   /* maximum number of open file descriptors */

/* ---- Special values ---- */
#define RLIM_INFINITY        0xFFFFFFFFU  /* no limit */
#define RLIM_NOFILE_DEFAULT  32U          /* default soft + hard (= VFS_MAX_FD) */
#define RLIM_NOFILE_HARD_MAX 32U          /* absolute ceiling – cannot be raised */

/* ---- Resource-limit pair ---- */
typedef struct {
    unsigned int rlim_cur;   /* soft limit  (enforced) */
    unsigned int rlim_max;   /* hard limit  (ceiling)  */
} rlimit_t;

#endif /* RLIMIT_H */
