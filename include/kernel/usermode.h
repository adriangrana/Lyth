#ifndef USERMODE_H
#define USERMODE_H

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
                                   unsigned int frame_esp);
/* Spawn a tiny synthetic user-mode task that intentionally touches the
   unmapped stack guard page to validate overflow detection. */
int usermode_spawn_stackbomb(int foreground);

#endif