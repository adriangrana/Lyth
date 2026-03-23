#ifndef USERMODE_H
#define USERMODE_H

#include <stdint.h>

/* Spawn an ELF task from the legacy flat FS (by name). */
int usermode_spawn_elf_task(const char* fs_name, int foreground);
/* Spawn an ELF task from an absolute VFS path. */
int usermode_spawn_elf_vfs(const char* vfs_path, int foreground);
/* Spawn an ELF task from an absolute VFS path with full argv/envp.
   argv[argc] and envp[envc] must be NULL-terminated C string arrays
   (or NULL to pass no arguments / no environment). */
int usermode_spawn_elf_vfs_argv(const char* vfs_path,
                                int argc, const char* const* argv,
                                int envc, const char* const* envp,
                                int foreground);
/* Replace current user task image with a VFS ELF (exec semantics). */
int usermode_exec_current_vfs_argv(const char* vfs_path,
                                   int argc, const char* const* argv,
                                   int envc, const char* const* envp,
                                   uintptr_t frame_rsp);
/* Spawn a tiny synthetic user-mode task that intentionally touches the
   unmapped stack guard page to validate overflow detection. */
int usermode_spawn_stackbomb(int foreground);
/* Spawn a tiny synthetic user-mode task that touches valid stack memory
   and exits cleanly, used as a control test for stack guard pages. */
int usermode_spawn_stackok(int foreground);
int usermode_spawn_shm_writer(int segment_id, unsigned char value, int foreground);
int usermode_spawn_shm_reader(int segment_id, unsigned char expected, int foreground);

#endif