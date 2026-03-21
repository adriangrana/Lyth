#ifndef UGDB_H
#define UGDB_H

/* ---- Limits ---- */
#define UGDB_MAX_USERS  8U
#define UGDB_MAX_GROUPS 8U
#define UGDB_NAME_MAX   16U

/* ---- Built-in IDs ---- */
#define UGDB_UID_ROOT  0U   /* superuser */
#define UGDB_GID_WHEEL 0U   /* root group */

/* ---- User entry ---- */
typedef struct {
    int          used;
    unsigned int uid;
    unsigned int gid;   /* primary group */
    char         name[UGDB_NAME_MAX];
} ugdb_user_t;

/* ---- Group entry ---- */
typedef struct {
    int          used;
    unsigned int gid;
    char         name[UGDB_NAME_MAX];
} ugdb_group_t;

/* Initialise the database with built-in root/user entries.
   Must be called once, before any lookup. */
void ugdb_init(void);

/* Lookup by numeric id or by name.  Returns pointer to the internal
   (static) entry, or NULL if not found. */
const ugdb_user_t*  ugdb_find_by_uid (unsigned int uid);
const ugdb_user_t*  ugdb_find_by_name(const char* name);
const ugdb_group_t* ugdb_find_group_by_gid (unsigned int gid);
const ugdb_group_t* ugdb_find_group_by_name(const char* name);

/* Convenience: human-readable name for a uid/gid.
   Returns "unknown" when no entry matches. */
const char* ugdb_username (unsigned int uid);
const char* ugdb_groupname(unsigned int gid);

/* Returns the next free uid/gid >= 1000, or 0xFFFFFFFF if the table is full. */
unsigned int ugdb_next_uid(void);
unsigned int ugdb_next_gid(void);

/* Add a new user or group.  Returns 0 on success, -1 on error. */
int ugdb_add_user (unsigned int uid, unsigned int gid, const char* name);
int ugdb_add_group(unsigned int gid, const char* name);

#endif /* UGDB_H */
