#ifndef SYSCALL_H
#define SYSCALL_H

#include "signal.h"
#include "rlimit.h"

enum {
    SYSCALL_WRITE = 1,
    SYSCALL_GET_TICKS = 2,
    SYSCALL_SLEEP = 3,
    SYSCALL_YIELD = 4,
    SYSCALL_ALLOC = 5,
    SYSCALL_FREE = 6,
    SYSCALL_FS_COUNT = 7,
    SYSCALL_FS_NAME_AT = 8,
    SYSCALL_FS_READ = 9,
    SYSCALL_FS_SIZE = 10,
    SYSCALL_EXIT = 11,
    SYSCALL_FS_NAME_COPY = 12,
    /* VFS file-descriptor syscalls */
    SYSCALL_VFS_OPEN     = 13,  /* open(path)              -> fd   */
    SYSCALL_VFS_CLOSE    = 14,  /* close(fd)               -> 0    */
    SYSCALL_VFS_READ     = 15,  /* read(fd, buf, size)     -> n    */
    SYSCALL_VFS_WRITE    = 16,  /* write(fd, buf, size)    -> n    */
    SYSCALL_VFS_SEEK     = 17,  /* seek(fd, off, whence)   -> off  */
    SYSCALL_VFS_READDIR  = 18,  /* readdir(fd,idx,buf,max) -> 0/-1 */
    SYSCALL_VFS_CREATE   = 19,  /* create(path, flags)     -> 0/-1 */
    SYSCALL_VFS_DELETE   = 20,  /* delete(path)            -> 0/-1 */
    /* Process management */
    SYSCALL_GETPID       = 21,  /* getpid()                -> pid  */
    SYSCALL_KILL         = 22,  /* kill(id)                -> 0/-1 */
    SYSCALL_WAIT         = 23,  /* wait(id)                -> 0    */
    SYSCALL_EXEC         = 24,  /* exec(path, fg)          -> id   */
    SYSCALL_GET_ERRNO    = 25,  /* get_errno()             -> errno */
    SYSCALL_FORK         = 26,  /* fork()                  -> child_pid / 0 / -1 */
    SYSCALL_EXECV        = 27,  /* execv(path,fg,argv,argc) -> id / -1 */
    SYSCALL_SIGNAL       = 28,  /* signal(sig, handler)     -> old_handler / -1 */
    SYSCALL_KILLSIG      = 29,  /* kill(pid, sig)           -> 0 / -1 */
    SYSCALL_SIGPENDING   = 30,  /* sigpending()             -> bitmask */
    SYSCALL_SIGPROCMASK  = 31,  /* sigprocmask(how, mask)   -> oldmask / -1 */
    SYSCALL_WAITPID      = 32,  /* waitpid(pid, *status)    -> child_pid / -1 */
    SYSCALL_EXECVE       = 33,  /* execve(path, argv, envp) -> 0 / -1  (in-place) */
    SYSCALL_GET_TIME     = 34,  /* get_time(rtc_time_t* buf) -> 0 / -1 */
    SYSCALL_GET_MONOTONIC_MS = 35, /* get_monotonic_ms()      -> ms since boot */
    SYSCALL_POLL         = 36,  /* poll(pfds, nfds, timeout_ms) -> ready_count */
    SYSCALL_SELECT       = 37,  /* select(nfds, *rmask, *wmask, timeout_ms) */
    SYSCALL_PIPE         = 38,  /* pipe(int fds[2], flags) */
    SYSCALL_GETRLIMIT    = 39,  /* getrlimit(resource, rlimit_t*) -> 0/-1 */
    SYSCALL_SETRLIMIT    = 40,  /* setrlimit(resource, const rlimit_t*) -> 0/-1 */
    SYSCALL_GETUID       = 41,
    SYSCALL_GETGID       = 42,
    SYSCALL_GETEUID      = 43,
    SYSCALL_GETEGID      = 44,
    SYSCALL_SETUID       = 45,
    SYSCALL_SETGID       = 46,
    SYSCALL_VFS_CHOWN    = 47,
    SYSCALL_VFS_GETOWNER = 48,
    SYSCALL_GETGROUPS    = 49,
    SYSCALL_SETGROUPS    = 50,
    SYSCALL_ALARM        = 51,  /* alarm(seconds)                       -> remaining_secs */
    SYSCALL_SETITIMER    = 52,  /* setitimer(value_us, interval_us)     -> 0/-1 */
    SYSCALL_GETITIMER    = 53   /* getitimer(*value_us_out,*interval_us_out) -> 0/-1 */
};

#define SYSCALL_POLLIN   0x0001U
#define SYSCALL_POLLOUT  0x0004U
#define SYSCALL_POLLERR  0x0008U

#define SYSCALL_PIPE_NONBLOCK 0x0001U

typedef struct {
    int fd;
    unsigned short events;
    unsigned short revents;
} syscall_pollfd_t;

typedef void (*sys_signal_handler_t)(int);
#define SYSCALL_SIG_DFL ((sys_signal_handler_t)0)
#define SYSCALL_SIG_IGN ((sys_signal_handler_t)1)

unsigned int syscall_callback(unsigned int number,
                              unsigned int arg0,
                              unsigned int arg1,
                              unsigned int arg2,
                              unsigned int arg3);
unsigned int syscall_exec_interrupt(unsigned int frame_esp,
                                    unsigned int path,
                                    unsigned int foreground);
unsigned int syscall_execv_interrupt(unsigned int frame_esp,
                                     unsigned int path,
                                     unsigned int foreground,
                                     unsigned int argv,
                                     unsigned int argc);
unsigned int syscall_execve_interrupt(unsigned int frame_esp,
                                      unsigned int path,
                                      unsigned int argv_ptr,
                                      unsigned int envp_ptr);
unsigned int syscall_invoke(unsigned int number,
                            unsigned int arg0,
                            unsigned int arg1,
                            unsigned int arg2,
                            unsigned int arg3);

void syscall_write(const char* text);
unsigned int syscall_get_ticks(void);
void syscall_sleep(unsigned int ticks);
void syscall_yield(void);
void* syscall_alloc(unsigned int size);
void syscall_free(void* ptr);
void syscall_exit(int exit_code);
int syscall_fs_count(void);
int syscall_fs_name(int index, char* buffer, unsigned int buffer_size);
int syscall_fs_read(const char* name, char* buffer, unsigned int buffer_size);
unsigned int syscall_fs_size(const char* name);

/* VFS / file-descriptor helpers */
int  syscall_vfs_open   (const char* path);
int  syscall_vfs_open_flags(const char* path, unsigned int open_flags);
void syscall_vfs_close  (int fd);
int  syscall_vfs_read   (int fd, unsigned char* buf, unsigned int size);
int  syscall_vfs_write  (int fd, const unsigned char* buf, unsigned int size);
int  syscall_vfs_seek   (int fd, int offset, int whence);
int  syscall_vfs_readdir(int fd, unsigned int index,
                         char* name_out, unsigned int name_max);
int  syscall_vfs_create (const char* path, unsigned int flags);
int  syscall_vfs_delete (const char* path);

/* Process management helpers */
int  syscall_getpid(void);
int  syscall_kill(int id);
int  syscall_waitpid(int pid, int* status);
void syscall_wait(int id);
int  syscall_exec(const char* path, int foreground);
int  syscall_get_errno(void);
int  syscall_fork(void);
/* exec with explicit argv array */
int  syscall_execv(const char* path, int foreground,
                   const char* const* argv, int argc);
/* exec with argv + envp (NULL-terminated arrays, POSIX-style) */
int  syscall_execve(const char* path,
                    const char* const* argv,
                    const char* const* envp);
sys_signal_handler_t syscall_signal(int signum, sys_signal_handler_t handler);
int  syscall_killsig(int id, int signum);
unsigned int syscall_sigpending(void);
int  syscall_sigprocmask(unsigned int how, unsigned int mask, unsigned int* old_mask_out);

/* Time / clock helpers */
struct rtc_time_t_fwd;  /* forward-declared; include rtc.h for full type */
int          syscall_get_time(void* rtc_time_buf); /* fills rtc_time_t*  */
unsigned int syscall_get_monotonic_ms(void);       /* µs-precision mono  */

/* I/O multiplexing */
int syscall_poll(syscall_pollfd_t* pfds, unsigned int nfds, unsigned int timeout_ms);
int syscall_select(unsigned int nfds,
                   unsigned int* read_mask_io,
                   unsigned int* write_mask_io,
                   unsigned int timeout_ms);
int syscall_pipe(int fds_out[2], unsigned int flags);
int syscall_getrlimit(int resource, rlimit_t* rl);
int syscall_setrlimit(int resource, const rlimit_t* rl);
unsigned int syscall_getuid(void);
unsigned int syscall_getgid(void);
unsigned int syscall_geteuid(void);
unsigned int syscall_getegid(void);
int syscall_setuid(unsigned int uid);
int syscall_setgid(unsigned int gid);
int syscall_vfs_chown(const char* path, unsigned int uid, unsigned int gid);
int syscall_vfs_getowner(const char* path, unsigned int* uid_out, unsigned int* gid_out);
int syscall_getgroups(int max_groups, unsigned int* gids_out);
int syscall_setgroups(int count, const unsigned int* gids);

/* Timers per-process (ITIMER_REAL / alarm) */
typedef struct {
    unsigned int value_us;    /* time until next SIGALRM (0 = disarmed) */
    unsigned int interval_us; /* periodic reload (0 = one-shot) */
} syscall_itimerval_t;

unsigned int syscall_alarm(unsigned int seconds);
int syscall_setitimer(unsigned int value_us, unsigned int interval_us,
                      syscall_itimerval_t* old_out);
int syscall_getitimer(syscall_itimerval_t* out);

#endif
