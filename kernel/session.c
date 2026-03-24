/* ============================================================
 *  session.c  —  User session management
 *
 *  A session is created after the user authenticates at the login
 *  screen and destroyed on logout.  It holds the uid/gid context
 *  used by the desktop and all applications running within.
 * ============================================================ */

#include "session.h"
#include "ugdb.h"
#include "string.h"
#include "klog.h"

static session_t current_session;

/* ---- helpers ---- */

static void str_copy(char* dst, const char* src, int max) {
    int i = 0;
    while (src[i] && i + 1 < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

/* ---- public API ---- */

void session_init(void) {
    current_session.active = 0;
    current_session.uid = 0;
    current_session.gid = 0;
    current_session.username[0] = '\0';
    current_session.home[0] = '\0';
}

int session_create(unsigned int uid) {
    const ugdb_user_t* user = ugdb_find_by_uid(uid);
    if (!user) return -1;

    current_session.active = 1;
    current_session.uid = user->uid;
    current_session.gid = user->gid;
    str_copy(current_session.username, user->name, SESSION_NAME_MAX);

    /* Build home directory path: /root for uid 0, /home/<name> otherwise */
    if (uid == 0) {
        str_copy(current_session.home, "/root", SESSION_HOME_MAX);
    } else {
        char path[SESSION_HOME_MAX];
        int pos = 0;
        const char* prefix = "/home/";
        while (*prefix && pos + 1 < SESSION_HOME_MAX) path[pos++] = *prefix++;
        const char* n = user->name;
        while (*n && pos + 1 < SESSION_HOME_MAX) path[pos++] = *n++;
        path[pos] = '\0';
        str_copy(current_session.home, path, SESSION_HOME_MAX);
    }

    klog_write(KLOG_LEVEL_INFO, "session", "Session created");
    return 0;
}

void session_destroy(void) {
    if (current_session.active) {
        klog_write(KLOG_LEVEL_INFO, "session", "Session destroyed");
    }
    current_session.active = 0;
    current_session.uid = 0;
    current_session.gid = 0;
    current_session.username[0] = '\0';
    current_session.home[0] = '\0';
}

int session_is_active(void) {
    return current_session.active;
}

const session_t* session_get_current(void) {
    return current_session.active ? &current_session : 0;
}
