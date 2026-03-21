#include "ugdb.h"

/* ============================================================
 *  Internal tables
 * ============================================================ */

static ugdb_user_t  g_users [UGDB_MAX_USERS];
static ugdb_group_t g_groups[UGDB_MAX_GROUPS];

/* ---- String helpers (no stdlib available) ---- */

static void ugdb_str_copy(char* dst, const char* src, unsigned int max) {
    unsigned int i = 0;
    if (!dst || !src || max == 0) return;
    while (src[i] != '\0' && i + 1U < max) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static int ugdb_str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a != '\0' && *b != '\0' && *a == *b) { a++; b++; }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

/* ============================================================
 *  Init
 * ============================================================ */

void ugdb_init(void) {
    unsigned int i;
    for (i = 0; i < UGDB_MAX_USERS;  i++) g_users[i].used  = 0;
    for (i = 0; i < UGDB_MAX_GROUPS; i++) g_groups[i].used = 0;

    /* Built-in groups */
    ugdb_add_group(0, "wheel");
    ugdb_add_group(1, "users");

    /* Built-in users: root (uid=0, gid=0) and a regular user (uid=1, gid=1) */
    ugdb_add_user(0, 0, "root");
    ugdb_add_user(1, 1, "user");
}

/* ============================================================
 *  Add
 * ============================================================ */

int ugdb_add_group(unsigned int gid, const char* name) {
    unsigned int i;
    if (!name) return -1;
    for (i = 0; i < UGDB_MAX_GROUPS; i++) {
        if (!g_groups[i].used) {
            g_groups[i].used = 1;
            g_groups[i].gid  = gid;
            ugdb_str_copy(g_groups[i].name, name, UGDB_NAME_MAX);
            return 0;
        }
    }
    return -1; /* table full */
}

int ugdb_add_user(unsigned int uid, unsigned int gid, const char* name) {
    unsigned int i;
    if (!name) return -1;
    for (i = 0; i < UGDB_MAX_USERS; i++) {
        if (!g_users[i].used) {
            g_users[i].used = 1;
            g_users[i].uid  = uid;
            g_users[i].gid  = gid;
            ugdb_str_copy(g_users[i].name, name, UGDB_NAME_MAX);
            return 0;
        }
    }
    return -1;
}

/* ============================================================
 *  Lookup
 * ============================================================ */

const ugdb_user_t* ugdb_find_by_uid(unsigned int uid) {
    unsigned int i;
    for (i = 0; i < UGDB_MAX_USERS; i++)
        if (g_users[i].used && g_users[i].uid == uid) return &g_users[i];
    return 0;
}

const ugdb_user_t* ugdb_find_by_name(const char* name) {
    unsigned int i;
    if (!name) return 0;
    for (i = 0; i < UGDB_MAX_USERS; i++)
        if (g_users[i].used && ugdb_str_eq(g_users[i].name, name)) return &g_users[i];
    return 0;
}

const ugdb_group_t* ugdb_find_group_by_gid(unsigned int gid) {
    unsigned int i;
    for (i = 0; i < UGDB_MAX_GROUPS; i++)
        if (g_groups[i].used && g_groups[i].gid == gid) return &g_groups[i];
    return 0;
}

const ugdb_group_t* ugdb_find_group_by_name(const char* name) {
    unsigned int i;
    if (!name) return 0;
    for (i = 0; i < UGDB_MAX_GROUPS; i++)
        if (g_groups[i].used && ugdb_str_eq(g_groups[i].name, name)) return &g_groups[i];
    return 0;
}

const char* ugdb_username(unsigned int uid) {
    const ugdb_user_t* u = ugdb_find_by_uid(uid);
    return u ? u->name : "unknown";
}

const char* ugdb_groupname(unsigned int gid) {
    const ugdb_group_t* g = ugdb_find_group_by_gid(gid);
    return g ? g->name : "unknown";
}

/* ============================================================
 *  Next free id
 * ============================================================ */

unsigned int ugdb_next_uid(void) {
    unsigned int c = 1000U;
    unsigned int i;
    while (c < 0xFFFFFFFEU) {
        int found = 0;
        for (i = 0; i < UGDB_MAX_USERS; i++)
            if (g_users[i].used && g_users[i].uid == c) { found = 1; break; }
        if (!found) return c;
        c++;
    }
    return 0xFFFFFFFFU;
}

unsigned int ugdb_next_gid(void) {
    unsigned int c = 1000U;
    unsigned int i;
    while (c < 0xFFFFFFFEU) {
        int found = 0;
        for (i = 0; i < UGDB_MAX_GROUPS; i++)
            if (g_groups[i].used && g_groups[i].gid == c) { found = 1; break; }
        if (!found) return c;
        c++;
    }
    return 0xFFFFFFFFU;
}
