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
    char         password[16];  /* plaintext; empty string = no password */
} ugdb_user_t;

/* ---- Group entry ---- */
typedef struct {
    int          used;
    unsigned int gid;
    char         name[UGDB_NAME_MAX];
    unsigned int member_uids[UGDB_MAX_USERS];
    unsigned int member_count;
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

/* Remove a user/group by id.  Root (uid=0) and wheel (gid=0) are protected. */
int ugdb_del_user (unsigned int uid);
int ugdb_del_group(unsigned int gid);

/* Password management.
   ugdb_check_password: returns 1 on match; empty stored password always grants access. */
int ugdb_set_password  (unsigned int uid, const char* password);
int ugdb_check_password(unsigned int uid, const char* password);

/* Mutate user attributes in-place. */
int ugdb_set_user_gid (unsigned int uid, unsigned int new_gid);
int ugdb_set_user_name(unsigned int uid, const char* new_name);

/* Group membership. */
int ugdb_group_add_member   (unsigned int gid, unsigned int uid);
int ugdb_group_remove_member(unsigned int gid, unsigned int uid);
int ugdb_group_is_member    (unsigned int gid, unsigned int uid);

/* Fill gids_out with every group uid is a member of.  Returns count. */
int ugdb_get_user_groups(unsigned int uid, unsigned int* gids_out, int max);

#endif /* UGDB_H */
