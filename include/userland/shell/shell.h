#ifndef SHELL_H
#define SHELL_H

void        shell_init(void);
int         shell_execute_line(const char* line);
int         shell_complete_command(const char* prefix, const char* matches[], int max_matches);
int         shell_complete_path(const char* prefix, const char* matches[], int max_matches);
const char* shell_get_cwd(void);
const char* shell_get_current_user(void);

#endif