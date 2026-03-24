#ifndef SESSION_H
#define SESSION_H

#define SESSION_HOME_MAX  64
#define SESSION_NAME_MAX  16

/* User session descriptor */
typedef struct {
    int          active;
    unsigned int uid;
    unsigned int gid;
    char         username[SESSION_NAME_MAX];
    char         home[SESSION_HOME_MAX];
} session_t;

/* Initialise session subsystem (call once at boot). */
void session_init(void);

/* Create a session for the given uid.  Returns 0 on success, -1 on error. */
int  session_create(unsigned int uid);

/* Destroy the current session (logout). */
void session_destroy(void);

/* Query */
int              session_is_active(void);
const session_t* session_get_current(void);

#endif /* SESSION_H */
