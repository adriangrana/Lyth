#include "shell.h"
#include "terminal.h"
#include "fbconsole.h"
#include "string.h"
#include "shell_input.h"
#include "parser.h"
#include "task.h"
#include "timer.h"
#include "heap.h"
#include "syscall.h"
#include "physmem.h"
#include "paging.h"
#include "fs.h"
#include "vfs.h"
#include "elf.h"
#include "usermode.h"
#include "klog.h"
#include "mouse.h"
#include "ata.h"
#include "blkdev.h"
#include "fat16.h"
#include "fat32.h"
#include "rtc.h"
#include "rlimit.h"
#include "version.h"
#include "ugdb.h"

#define SHELL_MAX_ARGS 8
#define SHELL_TOKEN_MAX 64
#define SHELL_ENV_MAX 16
#define SHELL_ENV_NAME_MAX 16
#define SHELL_ENV_VALUE_MAX 64
#define SHELL_ENV_ENTRY_MAX (SHELL_ENV_NAME_MAX + SHELL_ENV_VALUE_MAX + 2)
#define SHELL_PIPE_MAX 4096
#define SHELL_REDIR_MAX 8

typedef int (*command_fn)(int argc, const char* argv[], int background);

typedef struct {
    const char* name;
    command_fn fn;
    const char* help;
} command_t;

typedef struct {
    int current;
    int limit;
    unsigned int delay_ticks;
} count_task_data_t;

typedef struct {
    unsigned int delay_ticks;
    int armed;
} sleep_task_data_t;

typedef struct {
    int event_id;
    int armed;
} wait_task_data_t;

typedef struct {
    int queue_id;
    unsigned int timeout_ticks;
    unsigned int deadline_tick;
    unsigned int payload_size;
    int wait_started;
    char payload[MQ_MAX_MESSAGE_SIZE];
} mq_send_wait_task_data_t;

typedef struct {
    int queue_id;
    unsigned int timeout_ticks;
    unsigned int deadline_tick;
    int wait_started;
} mq_recv_wait_task_data_t;

typedef struct {
    int queue_id;
    unsigned int delay_ticks;
    unsigned int payload_size;
    int armed;
    char payload[MQ_MAX_MESSAGE_SIZE];
} mq_delayed_send_task_data_t;

typedef struct {
    int queue_id;
    int phase;
    unsigned int recv_deadline_tick;
    unsigned int send_deadline_tick;
} mq_demo_task_data_t;

typedef struct {
    int used;
    char name[SHELL_ENV_NAME_MAX];
    char value[SHELL_ENV_VALUE_MAX];
} shell_env_var_t;

typedef struct {
    int used;
    unsigned int length;
    char name[SHELL_TOKEN_MAX];
    char content[SHELL_PIPE_MAX];
} shell_redir_file_t;

static int cmd_help(int argc, const char* argv[], int background);
static int cmd_clear(int argc, const char* argv[], int background);
static int cmd_about(int argc, const char* argv[], int background);
static int cmd_echo(int argc, const char* argv[], int background);
static int cmd_env(int argc, const char* argv[], int background);
static int cmd_set(int argc, const char* argv[], int background);
static int cmd_unset(int argc, const char* argv[], int background);
static int cmd_keymap(int argc, const char* argv[], int background);
static int cmd_color(int argc, const char* argv[], int background);
static int cmd_theme(int argc, const char* argv[], int background);
static int cmd_history(int argc, const char* argv[], int background);
static int cmd_count(int argc, const char* argv[], int background);
static int cmd_sleep(int argc, const char* argv[], int background);
static int cmd_uptime(int argc, const char* argv[], int background);
static int cmd_date(int argc, const char* argv[], int background);
static int cmd_ps(int argc, const char* argv[], int background);
static int cmd_kill(int argc, const char* argv[], int background);
static int cmd_nice(int argc, const char* argv[], int background);
static int cmd_renice(int argc, const char* argv[], int background);
static int cmd_killall(int argc, const char* argv[], int background);
static int cmd_pidof(int argc, const char* argv[], int background);
static int cmd_fg(int argc, const char* argv[], int background);
static int cmd_bg(int argc, const char* argv[], int background);
static int cmd_time(int argc, const char* argv[], int background);
static int cmd_alarm(int argc, const char* argv[], int background);
static int cmd_stackbomb(int argc, const char* argv[], int background);
static int cmd_stackok(int argc, const char* argv[], int background);
static int cmd_shm(int argc, const char* argv[], int background);
static int cmd_shmdemo(int argc, const char* argv[], int background);
static int cmd_mq(int argc, const char* argv[], int background);
static int cmd_task(int argc, const char* argv[], int background);
static int cmd_mem(int argc, const char* argv[], int background);
static int cmd_wait(int argc, const char* argv[], int background);
static int cmd_signal(int argc, const char* argv[], int background);
static int cmd_mouse(int argc, const char* argv[], int background);
static int cmd_dmesg(int argc, const char* argv[], int background);
static int cmd_gfxdemo(int argc, const char* argv[], int background);
static int cmd_ulimit(int argc, const char* argv[], int background);
static int cmd_ls(int argc, const char* argv[], int background);
static int cmd_cat(int argc, const char* argv[], int background);
static int cmd_grep(int argc, const char* argv[], int background);
static int cmd_source(int argc, const char* argv[], int background);
static int cmd_elfinfo(int argc, const char* argv[], int background);
static int cmd_exec(int argc, const char* argv[], int background);
static int cmd_repeat(int argc, const char* argv[], int background);
static int cmd_yield(int argc, const char* argv[], int background);
static int cmd_vfs(int argc, const char* argv[], int background);
static int cmd_disk(int argc, const char* argv[], int background);
static int cmd_cd(int argc, const char* argv[], int background);
static int cmd_pwd(int argc, const char* argv[], int background);
static int cmd_touch(int argc, const char* argv[], int background);
static int cmd_rm(int argc, const char* argv[], int background);
static int cmd_unlink(int argc, const char* argv[], int background);
static int cmd_stat(int argc, const char* argv[], int background);
static int cmd_mkdir(int argc, const char* argv[], int background);
static int cmd_chmod(int argc, const char* argv[], int background);
static int cmd_chown(int argc, const char* argv[], int background);
static int cmd_cp(int argc, const char* argv[], int background);
static int cmd_mv(int argc, const char* argv[], int background);
static int cmd_rename(int argc, const char* argv[], int background);
static int cmd_getpid(int argc, const char* argv[], int background);
static int cmd_whoami(int argc, const char* argv[], int background);
static int cmd_id(int argc, const char* argv[], int background);
static int cmd_su(int argc, const char* argv[], int background);
static int cmd_groups(int argc, const char* argv[], int background);
static int cmd_passwd(int argc, const char* argv[], int background);
static int cmd_useradd(int argc, const char* argv[], int background);
static int cmd_userdel(int argc, const char* argv[], int background);
static int cmd_usermod(int argc, const char* argv[], int background);
static int cmd_groupadd(int argc, const char* argv[], int background);
static int cmd_groupdel(int argc, const char* argv[], int background);
static int cmd_gpasswd(int argc, const char* argv[], int background);
static int cmd_login(int argc, const char* argv[], int background);
static int cmd_logout(int argc, const char* argv[], int background);
static int cmd_who(int argc, const char* argv[], int background);
static int cmd_users(int argc, const char* argv[], int background);
static int shell_read_secret_line(char* buf, unsigned int max);
static int cmd_rmdir(int argc, const char* argv[], int background);
static int cmd_find(int argc, const char* argv[], int background);
static int cmd_which(int argc, const char* argv[], int background);
static int cmd_head(int argc, const char* argv[], int background);
static int cmd_tail(int argc, const char* argv[], int background);
static int cmd_more(int argc, const char* argv[], int background);
static int cmd_wc(int argc, const char* argv[], int background);
static int cmd_cmp(int argc, const char* argv[], int background);
static int cmd_file(int argc, const char* argv[], int background);
static int cmd_du(int argc, const char* argv[], int background);
static int cmd_df(int argc, const char* argv[], int background);
static int cmd_sync(int argc, const char* argv[], int background);
static void shell_resolve_path(const char* input, char* out, unsigned int out_size);

/* Current working directory (always an absolute VFS path). */
static char shell_cwd[VFS_PATH_MAX];

const char* shell_get_cwd(void) { return shell_cwd; }

const char* shell_get_current_user(void) {
    const char* name = ugdb_username(task_current_euid());
    return (name && name[0] != '\0') ? name : "user";
}

static command_t commands[] = {
    {"help",    cmd_help,    "muestra esta ayuda"},
    {"clear",   cmd_clear,   "limpia la pantalla"},
    {"cls",     cmd_clear,   "alias de clear"},
    {"about",   cmd_about,   "informacion del sistema"},
    {"echo",    cmd_echo,    "muestra texto (soporta comillas)"},
    {"env",     cmd_env,     "lista variables de entorno"},
    {"set",     cmd_set,     "define variable: set <NOMBRE> <VALOR>"},
    {"unset",   cmd_unset,   "borra variable: unset <NOMBRE>"},
    {"keymap",  cmd_keymap,  "muestra/cambia layout: keymap [us|es]"},
    {"color",   cmd_color,   "cambia color: color green|red|white|blue"},
    {"theme",   cmd_theme,   "aplica tema visual: theme [default|matrix|amber|ice]"},
    {"history", cmd_history, "historial gestionado por la capa de entrada"},
    {"count",   cmd_count,   "demo programada; soporta '&' y Ctrl+C"},
    {"sleep",   cmd_sleep,   "duerme en ms; soporta '&'"},
    {"uptime",  cmd_uptime,  "muestra ticks y tiempo desde arranque"},
    {"date",    cmd_date,    "muestra la fecha y hora actual (RTC)"},
    {"ps",      cmd_ps,      "lista tareas activas"},
    {"jobs",    cmd_ps,      "alias de ps"},
    {"kill",    cmd_kill,    "envia señal: kill <id> [signum] (default SIGTERM=15)"},
    {"killall", cmd_killall, "mata tareas por nombre: killall <nombre> [signum]"},
    {"pidof",   cmd_pidof,   "muestra PIDs por nombre: pidof <nombre>"},
    {"nice",    cmd_nice,    "cambia prioridad: nice <id> <high|normal|low>"},
    {"renice",  cmd_renice,  "cambia prioridad: renice <high|normal|low> <id>"},
    {"fg",      cmd_fg,      "trae tarea al foreground: fg [id]"},
    {"bg",      cmd_bg,      "lista/mueve tareas al background: bg [id]"},
    {"time",    cmd_time,    "mide tiempo de ejecucion: time <comando> [args...]"},
    {"stackbomb", cmd_stackbomb, "prueba guard page del stack userland"},
    {"stackok", cmd_stackok, "prueba acceso valido al stack userland"},
    {"shm",     cmd_shm,     "memoria compartida: shm [list|create <bytes>|unlink <id>]"},
    {"shmdemo", cmd_shmdemo, "valida SHM con writer/reader userland: shmdemo [byte]"},
    {"mq",      cmd_mq,      "message passing: mq [list|create|open|send|recv|sendwait|recvwait|unlink|demo]"},
    {"task",    cmd_task,    "muestra la tarea actual y foreground"},
    {"mem",     cmd_mem,     "estadisticas de heap, memoria fisica y paging"},
    {"wait",    cmd_wait,    "bloquea una tarea en evento: wait <id> [&]"},
    {"signal",  cmd_signal,  "despierta tareas bloqueadas: signal <id>"},
    {"mouse",   cmd_mouse,   "muestra estado del raton PS/2"},
    {"dmesg",   cmd_dmesg,   "muestra o limpia logs del kernel: dmesg [clear]"},
    {"gfxdemo", cmd_gfxdemo, "dibuja primitivas graficas en framebuffer"},
    {"ls",      cmd_ls,      "lista directorio VFS: ls [ruta]"},
    {"cat",     cmd_cat,     "muestra un archivo VFS: cat <ruta>"},
    {"grep",    cmd_grep,    "filtra lineas: grep <patron> [ruta]"},
    {"source",  cmd_source,  "ejecuta script del FS: source <NOMBRE>"},
    {"elfinfo", cmd_elfinfo, "inspecciona un ELF del FS: elfinfo <NOMBRE>"},
    {"exec",    cmd_exec,    "carga y ejecuta un ELF: exec <NOMBRE> [args...] [&]"},
    {"repeat",  cmd_repeat,  "repite un comando N veces: repeat <N> <comando...>"},
    {"getpid",  cmd_getpid,  "muestra el PID del proceso actual"},
    {"whoami",  cmd_whoami,  "muestra el usuario efectivo"},
    {"id",      cmd_id,      "muestra uid/gid/euid/egid"},
    {"groups",  cmd_groups,  "muestra grupos efectivos y suplementarios"},
    {"su",      cmd_su,      "cambia identidad efectiva: su <usuario|uid>"},
    {"yield",   cmd_yield,   "cede CPU al scheduler"},
    {"ulimit",  cmd_ulimit,  "muestra/cambia limites de recursos: ulimit [-n <valor>] [-H] [-S]"},
    {"vfs",     cmd_vfs,     "VFS: mounts, ls/cat/touch/rm/stat/cp/mv/rename: vfs <subcmd> ..."},
    {"disk",    cmd_disk,    "Bloques: disk [read <lba> [dev]] [mount <dev> <ruta>] [fsck <dev>] [gpt <dev>]"},
    {"cd",      cmd_cd,      "cambia directorio: cd [ruta]"},
    {"pwd",     cmd_pwd,     "muestra el directorio actual"},
    {"touch",   cmd_touch,   "crea archivo vacio (sin contenido): touch <ruta>"},
    {"rm",      cmd_rm,      "elimina archivo: rm <ruta>"},
    {"unlink",  cmd_unlink,  "alias de rm: unlink <ruta>"},
    {"stat",    cmd_stat,    "metadata basica: stat <ruta>"},
    {"mkdir",   cmd_mkdir,   "crea directorio: mkdir <ruta>"},
    {"chmod",   cmd_chmod,   "cambia permisos UNIX: chmod <modo_octal> <ruta>"},
    {"chown",   cmd_chown,   "cambia propietario: chown <uid|user> <gid|group> <ruta>"},
    {"cp",      cmd_cp,      "copia archivo: cp <origen> <destino>"},
    {"mv",      cmd_mv,      "mueve/renombra archivo: mv <origen> <destino>"},
    {"rename",  cmd_rename,  "renombra archivo: rename <origen> <destino>"},
    /* --- user/group administration --- */
    {"passwd",  cmd_passwd,  "cambia contraseña: passwd [usuario]"},
    {"useradd", cmd_useradd, "crea usuario: useradd <nombre> [-u uid] [-g gid]"},
    {"adduser", cmd_useradd, "alias de useradd"},
    {"userdel", cmd_userdel, "elimina usuario: userdel [-r] <usuario>"},
    {"deluser", cmd_userdel, "alias de userdel"},
    {"usermod", cmd_usermod, "modifica usuario: usermod -n <nombre>|-g <gid>|-p <pass>  <usuario>"},
    {"groupadd",cmd_groupadd,"crea grupo: groupadd <nombre> [-g gid]"},
    {"addgroup",cmd_groupadd,"alias de groupadd"},
    {"groupdel",cmd_groupdel,"elimina grupo: groupdel <nombre>"},
    {"delgroup",cmd_groupdel,"alias de groupdel"},
    {"gpasswd", cmd_gpasswd, "miembros de grupo: gpasswd -a|-d <usuario> <grupo>"},
    {"groupmem",cmd_gpasswd, "alias de gpasswd"},
    {"login",   cmd_login,   "inicia sesión: login <usuario>"},
    {"logout",  cmd_logout,  "cierra sesión actual y vuelve a root"},
    {"who",     cmd_who,     "muestra usuarios activos"},
    {"users",   cmd_users,   "lista nombres de usuarios conectados"},
    /* --- filesystem utilities --- */
    {"rmdir",   cmd_rmdir,   "borra directorio vacio: rmdir <ruta>"},
    {"find",    cmd_find,    "busca archivos: find <ruta> [-name <patron>]"},
    {"which",   cmd_which,   "localiza un comando: which <nombre>"},
    {"head",    cmd_head,    "primeras lineas: head [-n N] [ruta] o comando | head"},
    {"tail",    cmd_tail,    "ultimas lineas: tail [-n N] [ruta] o comando | tail"},
    {"more",    cmd_more,    "pagina texto: more [ruta] o comando | more"},
    {"less",    cmd_more,    "alias de more"},
    {"wc",      cmd_wc,      "cuenta lineas/palabras/bytes: wc [-l|-w|-c] [ruta] o comando | wc"},
    {"cmp",     cmd_cmp,     "compara dos archivos byte a byte: cmp [-l] [-v] [-q] <a> <b>"},
    {"diff",    cmd_cmp,     "alias de cmp"},
    {"file",    cmd_file,    "detecta tipo de archivo: file <ruta>"},
    {"du",      cmd_du,      "uso de disco por directorio: du [ruta]"},
    {"df",      cmd_df,      "espacio libre por filesystem: df"},
    {"sync",    cmd_sync,    "fuerza escritura pendiente a disco: sync"},
    {"alarm",   cmd_alarm,   "arma/cancela SIGALRM: alarm [segundos]"},
};

static const int command_count = sizeof(commands) / sizeof(commands[0]);
static shell_env_var_t shell_env[SHELL_ENV_MAX];
static char* shell_pipe_buffer = 0;
static unsigned int shell_pipe_length = 0;
static unsigned int shell_pipe_capacity = 0;
static int shell_pipe_active = 0;
static shell_redir_file_t shell_redir_files[SHELL_REDIR_MAX];
static const char* current_theme_name = "default";
static unsigned char current_theme_banner_color = 0x0B;
static unsigned char current_theme_hint_color = 0x07;
static char shell_storage_root[VFS_PATH_MAX] = "/";
static char shell_home_path[VFS_PATH_MAX];
static int shell_profile_loading = 0;

static void shell_apply_theme(const char* name, int print_feedback);
static int shell_find_unquoted_char(const char* text, char target);
static int shell_find_unquoted_andand(const char* text);
static void shell_trim_trailing_spaces(char* text);
static int shell_buffer_reserve(char** buffer, unsigned int* capacity, unsigned int required);
static void shell_set_pipe_input_owned(char* text, unsigned int length);
static int shell_set_pipe_input(const char* text);
static void shell_clear_pipe_input(void);
static int shell_has_pipe_input(void);
static int shell_read_fd_fully(int fd, char** out_buffer, unsigned int* out_length);
static int shell_load_text_argument_or_pipe(const char* path_arg,
                                            char** out_buffer,
                                            unsigned int* out_length,
                                            char* resolved_path,
                                            unsigned int resolved_size);
static int shell_capture_command_output(const char* line, char** out_buffer, unsigned int* out_length);
static int shell_execute_pipeline(const char* line);
static int shell_redir_find(const char* name);
static int shell_redir_store(const char* name, const char* content, unsigned int length, int append);
static int shell_read_text_source(const char* name, char* buffer, unsigned int buffer_size);
static int shell_env_set(const char* name, const char* value);
static const char* shell_env_get(const char* name);
static int shell_execute_script_text(const char* text);
static int shell_execute_line_raw(const char* line);
static unsigned int shell_parse_mode_octal(const char* s, int* ok);
static void shell_print_mode_octal(unsigned int mode);
static int shell_path_is_dir(const char* path);
static int shell_ensure_dir(const char* path);
static int shell_mkdir_p(const char* path);
static void shell_select_storage_root(void);
static void shell_build_rooted_path(const char* suffix, char* out, unsigned int out_size);
static void shell_build_user_home_path(const char* username, char* out, unsigned int out_size);
static void shell_append_text(char* dst, unsigned int max, unsigned int* pos, const char* src);
static void shell_write_user_profile(void);
static int shell_source_vfs_script(const char* path);
static void shell_apply_user_session(int load_global_rc);
static void shell_sanitize_cwd(void);

static int is_env_name_char(char c) {
    return (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') ||
           c == '_';
}

static void copy_bounded(char* dst, const char* src, int dst_size) {
    int index = 0;

    if (dst == 0 || src == 0 || dst_size <= 0) {
        return;
    }

    while (src[index] != '\0' && index < dst_size - 1) {
        dst[index] = src[index];
        index++;
    }

    dst[index] = '\0';
}

static int shell_path_is_dir(const char* path) {
    vfs_stat_t st;
    if (!path) return 0;
    if (vfs_stat(path, &st) != 0) return 0;
    return (st.flags & VFS_FLAG_DIR) ? 1 : 0;
}

static int shell_ensure_dir(const char* path) {
    if (!path || path[0] == '\0') return 0;
    if (shell_path_is_dir(path)) return 1;
    if (vfs_create(path, VFS_FLAG_DIR) == 0) return 1;
    return shell_path_is_dir(path);
}

static int shell_mkdir_p(const char* path) {
    char tmp[VFS_PATH_MAX];
    unsigned int i;
    unsigned int len = 0;

    if (!path || path[0] != '/') return 0;

    for (i = 0; path[i] != '\0' && i < VFS_PATH_MAX - 1U; i++) {
        tmp[i] = path[i];
        len = i + 1U;
    }
    tmp[len] = '\0';

    if (len == 0U) return 0;
    if (len == 1U && tmp[0] == '/') return 1;

    for (i = 1; i < len; i++) {
        if (tmp[i] != '/') continue;
        tmp[i] = '\0';
        if (!shell_ensure_dir(tmp)) return 0;
        tmp[i] = '/';
    }

    return shell_ensure_dir(tmp);
}

static void shell_select_storage_root(void) {
    /* Temporalmente fijamos la raiz de sesion a '/'.
       Las rutas bajo /hd0* pueden quedar en estados parciales (permisos/backend)
       y provocar fallos al listar directorios de HOME. */
    copy_bounded(shell_storage_root, "/", VFS_PATH_MAX);
}

static void shell_build_rooted_path(const char* suffix, char* out, unsigned int out_size) {
    unsigned int w = 0;
    unsigned int i;

    if (!out || out_size == 0U) return;
    out[0] = '\0';

    if (!suffix) return;

    if (str_equals(shell_storage_root, "/")) {
        out[w++] = '/';
    } else {
        for (i = 0; shell_storage_root[i] != '\0' && w + 1U < out_size; i++)
            out[w++] = shell_storage_root[i];
        if (w + 1U < out_size && out[w - 1U] != '/') out[w++] = '/';
    }

    for (i = (suffix[0] == '/') ? 1U : 0U; suffix[i] != '\0' && w + 1U < out_size; i++)
        out[w++] = suffix[i];

    out[w] = '\0';
}

static void shell_build_user_home_path(const char* username, char* out, unsigned int out_size) {
    char base[VFS_PATH_MAX];
    unsigned int w = 0;
    unsigned int i;

    shell_build_rooted_path("/home", base, sizeof(base));

    if (!out || out_size == 0U) return;
    out[0] = '\0';

    for (i = 0; base[i] != '\0' && w + 1U < out_size; i++) out[w++] = base[i];
    if (w + 1U < out_size && (w == 0U || out[w - 1U] != '/')) out[w++] = '/';
    for (i = 0; username && username[i] != '\0' && w + 1U < out_size; i++) out[w++] = username[i];
    out[w] = '\0';
}

static void shell_append_text(char* dst, unsigned int max, unsigned int* pos, const char* src) {
    unsigned int i;
    if (!dst || !pos || !src || max == 0U) return;
    for (i = 0; src[i] != '\0' && *pos + 1U < max; i++) {
        dst[*pos] = src[i];
        (*pos)++;
    }
    dst[*pos] = '\0';
}

static void shell_write_user_profile(void) {
    char profile_path[VFS_PATH_MAX];
    char body[256];
    const char* layout_name;
    unsigned int len = 0;
    int fd;

    if (shell_profile_loading) return;
    if (shell_home_path[0] == '\0') return;

    copy_bounded(profile_path, shell_home_path, VFS_PATH_MAX);
    {
        unsigned int p = str_length(profile_path);
        const char* suffix = "/.lythrc";
        unsigned int i;
        for (i = 0; suffix[i] != '\0' && p + 1U < VFS_PATH_MAX; i++) {
            profile_path[p++] = suffix[i];
        }
        profile_path[p] = '\0';
    }

    body[0] = '\0';
    shell_append_text(body, sizeof(body), &len, "theme ");
    shell_append_text(body, sizeof(body), &len, current_theme_name);
    shell_append_text(body, sizeof(body), &len, "\n");
    shell_append_text(body, sizeof(body), &len, "keymap ");
    layout_name = keyboard_layout_name(keyboard_get_layout());
    shell_append_text(body, sizeof(body), &len, layout_name ? layout_name : "us");
    shell_append_text(body, sizeof(body), &len, "\n");

    fd = vfs_open_flags(profile_path, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) return;
    vfs_write(fd, (const unsigned char*)body, len);
    vfs_close(fd);
}

static int shell_source_vfs_script(const char* path) {
    vfs_stat_t st;
    int fd;
    char* buf;
    unsigned int read_total = 0U;

    if (!path) return 0;
    if (vfs_stat(path, &st) != 0) return 0;
    if (st.flags & VFS_FLAG_DIR) return 0;

    fd = vfs_open_flags(path, VFS_O_RDONLY);
    if (fd < 0) return 0;

    buf = (char*)kmalloc(st.size + 1U);
    if (!buf) {
        vfs_close(fd);
        return 0;
    }

    while (read_total < st.size) {
        int n = vfs_read(fd,
                         (unsigned char*)(buf + read_total),
                         st.size - read_total);
        if (n <= 0) break;
        read_total += (unsigned int)n;
    }
    vfs_close(fd);
    buf[read_total] = '\0';

    shell_profile_loading = 1;
    shell_execute_script_text(buf);
    shell_profile_loading = 0;
    kfree(buf);
    return 1;
}

static void shell_apply_user_session(int load_global_rc) {
    char etc_path[VFS_PATH_MAX];
    char home_root[VFS_PATH_MAX];
    char global_rc[VFS_PATH_MAX];
    char user_rc[VFS_PATH_MAX];
    const char* username = ugdb_username(task_current_euid());
    int have_storage = 0;

    if (!username || username[0] == '\0') username = "user";

    shell_select_storage_root();

    shell_build_rooted_path("/etc", etc_path, sizeof(etc_path));
    shell_build_rooted_path("/home", home_root, sizeof(home_root));

    have_storage = shell_mkdir_p(etc_path) && shell_mkdir_p(home_root);

    shell_build_user_home_path(username, shell_home_path, sizeof(shell_home_path));
    have_storage = have_storage && shell_mkdir_p(shell_home_path);

    if (!have_storage || !shell_path_is_dir(shell_home_path)) {
        /* Fallback seguro: usar la raiz ramfs si el backend persistente
           no permite crear /home/<user> (p.ej. montaje RO o sin create). */
        copy_bounded(shell_storage_root, "/", VFS_PATH_MAX);

        shell_build_rooted_path("/etc", etc_path, sizeof(etc_path));
        shell_build_rooted_path("/home", home_root, sizeof(home_root));
        shell_build_user_home_path(username, shell_home_path, sizeof(shell_home_path));

        shell_mkdir_p(etc_path);
        shell_mkdir_p(home_root);
        shell_mkdir_p(shell_home_path);

        if (!shell_path_is_dir(shell_home_path)) {
            copy_bounded(shell_home_path, "/", VFS_PATH_MAX);
        }
    }

    /* Assign the home directory to the current user so they can write to it.
       This must happen while euid=0 (boot context or su before identity drop). */
    {
        const ugdb_user_t* u = ugdb_find_by_name(username);
        unsigned int home_uid = u ? u->uid : task_current_uid();
        unsigned int home_gid = u ? u->gid : task_current_gid();
        if (shell_home_path[0] != '\0' && !str_equals(shell_home_path, "/"))
            vfs_chown(shell_home_path, home_uid, home_gid);
    }

    copy_bounded(shell_cwd, shell_home_path, VFS_PATH_MAX);
    shell_env_set("HOME", shell_home_path);
    shell_env_set("USER", username);

    if (load_global_rc) {
        shell_build_rooted_path("/etc/bootrc.sh", global_rc, sizeof(global_rc));
        (void)shell_source_vfs_script(global_rc);
    }

    copy_bounded(user_rc, shell_home_path, sizeof(user_rc));
    {
        unsigned int p = str_length(user_rc);
        const char* suffix = "/.lythrc";
        unsigned int i;
        for (i = 0; suffix[i] != '\0' && p + 1U < sizeof(user_rc); i++) user_rc[p++] = suffix[i];
        user_rc[p] = '\0';
    }

    if (!shell_source_vfs_script(user_rc)) {
        shell_write_user_profile();
    }

    shell_sanitize_cwd();
}

static void shell_sanitize_cwd(void) {
    const char* home;
    if (shell_path_is_dir(shell_cwd)) return;

    home = shell_env_get("HOME");
    if (home && home[0] != '\0' && shell_path_is_dir(home)) {
        copy_bounded(shell_cwd, home, VFS_PATH_MAX);
        return;
    }

    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';
}

static int shell_env_find(const char* name) {
    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (shell_env[i].used && str_equals(shell_env[i].name, name)) {
            return i;
        }
    }

    return -1;
}

static const char* shell_env_get(const char* name) {
    int index = shell_env_find(name);

    if (index < 0) {
        return 0;
    }

    return shell_env[index].value;
}

static int shell_env_set(const char* name, const char* value) {
    int index;

    if (name == 0 || value == 0 || name[0] == '\0') {
        return 0;
    }

    for (int i = 0; name[i] != '\0'; i++) {
        if (!is_env_name_char(name[i]) || (i == 0 && name[i] >= '0' && name[i] <= '9')) {
            return 0;
        }
    }

    index = shell_env_find(name);
    if (index < 0) {
        for (int i = 0; i < SHELL_ENV_MAX; i++) {
            if (!shell_env[i].used) {
                index = i;
                shell_env[i].used = 1;
                break;
            }
        }
    }

    if (index < 0) {
        return 0;
    }

    copy_bounded(shell_env[index].name, name, SHELL_ENV_NAME_MAX);
    copy_bounded(shell_env[index].value, value, SHELL_ENV_VALUE_MAX);
    return 1;
}

static int shell_env_unset(const char* name) {
    int index = shell_env_find(name);

    if (index < 0) {
        return 0;
    }

    shell_env[index].used = 0;
    shell_env[index].name[0] = '\0';
    shell_env[index].value[0] = '\0';
    return 1;
}

/* Build envp[] as "NAME=VALUE" strings in caller-provided storage.
   Returns envc and NULL-terminates envp_out. */
static int shell_build_envp(const char* envp_out[],
                            char env_storage[][SHELL_ENV_ENTRY_MAX],
                            int max_env) {
    int envc = 0;

    if (!envp_out || !env_storage || max_env <= 0) {
        return 0;
    }

    for (int i = 0; i < SHELL_ENV_MAX && envc < max_env - 1; i++) {
        int w = 0;

        if (!shell_env[i].used) {
            continue;
        }

        for (int j = 0; shell_env[i].name[j] != '\0' && w < SHELL_ENV_ENTRY_MAX - 1; j++) {
            env_storage[envc][w++] = shell_env[i].name[j];
        }

        if (w < SHELL_ENV_ENTRY_MAX - 1) {
            env_storage[envc][w++] = '=';
        }

        for (int j = 0; shell_env[i].value[j] != '\0' && w < SHELL_ENV_ENTRY_MAX - 1; j++) {
            env_storage[envc][w++] = shell_env[i].value[j];
        }

        env_storage[envc][w] = '\0';
        envp_out[envc] = env_storage[envc];
        envc++;
    }

    envp_out[envc] = 0;
    return envc;
}

static void shell_expand_token(const char* input, char* output, int output_size) {
    int read_index = 0;
    int write_index = 0;

    if (output_size <= 0) {
        return;
    }

    while (input[read_index] != '\0' && write_index < output_size - 1) {
        if (input[read_index] == '$') {
            char name[SHELL_ENV_NAME_MAX];
            int name_length = 0;
            int name_index = read_index + 1;
            const char* value;

            while (is_env_name_char(input[name_index]) && name_length < SHELL_ENV_NAME_MAX - 1) {
                name[name_length++] = input[name_index++];
            }

            if (name_length > 0) {
                name[name_length] = '\0';
                value = shell_env_get(name);
                if (value != 0) {
                    for (int i = 0; value[i] != '\0' && write_index < output_size - 1; i++) {
                        output[write_index++] = value[i];
                    }
                }
                read_index = name_index;
                continue;
            }
        }

        output[write_index++] = input[read_index++];
    }

    output[write_index] = '\0';
}

static void shell_join_args_to_buffer(int argc,
                                      const char* argv[],
                                      int start_index,
                                      char* output,
                                      int output_size) {
    int write_index = 0;

    if (output == 0 || output_size <= 0) {
        return;
    }

    output[0] = '\0';

    for (int i = start_index; i < argc && write_index < output_size - 1; i++) {
        if (i > start_index && write_index < output_size - 1) {
            output[write_index++] = ' ';
        }

        for (int j = 0; argv[i][j] != '\0' && write_index < output_size - 1; j++) {
            output[write_index++] = argv[i][j];
        }
    }

    output[write_index] = '\0';
}

static void shell_print_text_with_color(const char* text, unsigned char color) {
    for (int i = 0; text[i] != '\0'; i++) {
        terminal_put_char_with_color(text[i], color);
    }
}

static unsigned int shell_uint_length(unsigned int value) {
    unsigned int digits = 1;

    while (value >= 10) {
        value /= 10;
        digits++;
    }

    return digits;
}

static void shell_print_int(int value) {
    if (value < 0) {
        terminal_put_char('-');
        terminal_print_uint((unsigned int)(-value));
        return;
    }

    terminal_print_uint((unsigned int)value);
}

static void shell_print_spaces(int count) {
    for (int i = 0; i < count; i++) {
        terminal_put_char(' ');
    }
}

static int shell_min_int(int a, int b) {
    return a < b ? a : b;
}

static int shell_max_int(int a, int b) {
    return a > b ? a : b;
}

static int shell_string_ends_with_ignore_case(const char* text, const char* suffix) {
    unsigned int text_length;
    unsigned int suffix_length;
    unsigned int offset;

    if (text == 0 || suffix == 0) {
        return 0;
    }

    text_length = str_length(text);
    suffix_length = str_length(suffix);
    if (suffix_length > text_length) {
        return 0;
    }

    offset = text_length - suffix_length;
    for (unsigned int i = 0; i < suffix_length; i++) {
        char a = text[offset + i];
        char b = suffix[i];

        if (a >= 'A' && a <= 'Z') {
            a = (char)(a - 'A' + 'a');
        }
        if (b >= 'A' && b <= 'Z') {
            b = (char)(b - 'A' + 'a');
        }

        if (a != b) {
            return 0;
        }
    }

    return 1;
}

static const char* shell_fs_entry_type(const char* name) {
    if (shell_string_ends_with_ignore_case(name, ".ELF")) {
        return "binary";
    }

    if (shell_string_ends_with_ignore_case(name, ".SH")) {
        return "script";
    }

    return "text";
}

static void shell_print_table_border(int name_width, int size_width, int type_width) {
    terminal_put_char('+');
    for (int i = 0; i < name_width + 2; i++) {
        terminal_put_char('-');
    }
    terminal_put_char('+');
    for (int i = 0; i < size_width + 2; i++) {
        terminal_put_char('-');
    }
    terminal_put_char('+');
    for (int i = 0; i < type_width + 2; i++) {
        terminal_put_char('-');
    }
    terminal_print_line("+");
}

static void shell_print_table_cell(const char* text, int width, int color) {
    int text_length = (int)str_length(text);

    terminal_put_char(' ');
    shell_print_text_with_color(text, (unsigned char)color);
    shell_print_spaces(width - text_length + 1);
    terminal_put_char('|');
}

static const char* shell_trim_leading_spaces(const char* text) {
    while (*text == ' ' || *text == '\t') {
        text++;
    }

    return text;
}

static int shell_execute_script_text(const char* text) {
    char line[128];
    int line_length = 0;

    if (text == 0) {
        return 0;
    }

    for (int index = 0;; index++) {
        char current = text[index];

        if (current == '\r') {
            continue;
        }

        if (current == '\n' || current == '\0') {
            const char* trimmed;

            line[line_length] = '\0';
            trimmed = shell_trim_leading_spaces(line);

            if (trimmed[0] != '\0' && trimmed[0] != '#') {
                shell_execute_line(trimmed);
            }

            line_length = 0;

            if (current == '\0') {
                break;
            }

            continue;
        }

        if (line_length < (int)sizeof(line) - 1) {
            line[line_length++] = current;
        }
    }

    return 1;
}

static void shell_print_banner(void) {
    int fd;
    shell_print_text_with_color("Lyth OS shell\n", current_theme_banner_color);
    shell_print_text_with_color("-------------\n", current_theme_banner_color);
    fd = vfs_open_flags("/etc/motd", VFS_O_RDONLY);
    if (fd >= 0) {
        char buf[256];
        int n;
        while ((n = vfs_read(fd, (unsigned char*)buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            terminal_print(buf);
        }
        vfs_close(fd);
    }
    shell_print_text_with_color("Escribe 'help' para ver comandos.\n", current_theme_hint_color);
    terminal_print_line("");
}

static int shell_find_unquoted_char(const char* text, char target) {
    int index = 0;
    char quote = 0;

    if (text == 0) {
        return -1;
    }

    while (text[index] != '\0') {
        char current = text[index];

        if (current == '\\' && text[index + 1] != '\0') {
            index += 2;
            continue;
        }

        if (quote != 0) {
            if (current == quote) {
                quote = 0;
            }
        } else if (current == '\'' || current == '"') {
            quote = current;
        } else if (current == target) {
            return index;
        }

        index++;
    }

    return -1;
}

static int shell_find_unquoted_andand(const char* text) {
    int index = 0;
    char quote = 0;

    if (text == 0) {
        return -1;
    }

    while (text[index] != '\0') {
        char current = text[index];

        if (current == '\\' && text[index + 1] != '\0') {
            index += 2;
            continue;
        }

        if (quote != 0) {
            if (current == quote) {
                quote = 0;
            }
            index++;
            continue;
        }

        if (current == '\'' || current == '"') {
            quote = current;
            index++;
            continue;
        }

        if (current == '&' && text[index + 1] == '&') {
            return index;
        }

        index++;
    }

    return -1;
}

static void shell_trim_trailing_spaces(char* text) {
    int length;

    if (text == 0) {
        return;
    }

    length = (int)str_length(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t')) {
        text[length - 1] = '\0';
        length--;
    }
}

static int shell_buffer_reserve(char** buffer, unsigned int* capacity, unsigned int required) {
    char* next_buffer;
    unsigned int next_capacity;
    unsigned int i;

    if (buffer == 0 || capacity == 0) {
        return 0;
    }

    if (required <= *capacity) {
        return 1;
    }

    next_capacity = *capacity > 0 ? *capacity : 64U;
    while (next_capacity < required) {
        if (next_capacity >= 0x80000000U) {
            next_capacity = required;
            break;
        }
        next_capacity *= 2U;
    }

    next_buffer = (char*)kmalloc(next_capacity);
    if (next_buffer == 0) {
        terminal_print_line("[error] Memoria insuficiente para procesar pipe/redireccion");
        return 0;
    }

    if (*buffer != 0) {
        for (i = 0; i < *capacity; i++) {
            next_buffer[i] = (*buffer)[i];
        }
        kfree(*buffer);
    }

    *buffer = next_buffer;
    *capacity = next_capacity;
    return 1;
}

static void shell_set_pipe_input_owned(char* text, unsigned int length) {
    shell_clear_pipe_input();

    if (text == 0) {
        return;
    }

    shell_pipe_buffer = text;
    shell_pipe_length = length;
    shell_pipe_capacity = length + 1U;
    shell_pipe_active = 1;
    shell_pipe_buffer[length] = '\0';
}

static int shell_set_pipe_input(const char* text) {
    unsigned int length;
    char* buffer = 0;
    unsigned int capacity = 0;
    unsigned int i;

    shell_clear_pipe_input();

    if (text == 0) {
        return 1;
    }

    length = str_length(text);
    if (!shell_buffer_reserve(&buffer, &capacity, length + 1U)) {
        return 0;
    }

    for (i = 0; i < length; i++) {
        buffer[i] = text[i];
    }

    buffer[length] = '\0';
    shell_pipe_buffer = buffer;
    shell_pipe_length = length;
    shell_pipe_capacity = capacity;
    shell_pipe_active = 1;
    return 1;
}

static void shell_clear_pipe_input(void) {
    if (shell_pipe_buffer != 0) {
        kfree(shell_pipe_buffer);
    }
    shell_pipe_buffer = 0;
    shell_pipe_length = 0;
    shell_pipe_capacity = 0;
    shell_pipe_active = 0;
}

static int shell_has_pipe_input(void) {
    return shell_pipe_active;
}

static int shell_read_fd_fully(int fd, char** out_buffer, unsigned int* out_length) {
    char* buffer = 0;
    unsigned int capacity = 0;
    unsigned int length = 0;
    int n;

    if (out_buffer == 0 || out_length == 0) {
        return 0;
    }

    *out_buffer = 0;
    *out_length = 0;

    if (!shell_buffer_reserve(&buffer, &capacity, 64U)) {
        return 0;
    }

    while ((n = vfs_read(fd, (unsigned char*)buffer + length, capacity - length - 1U)) > 0) {
        length += (unsigned int)n;
        if (!shell_buffer_reserve(&buffer, &capacity, length + 65U)) {
            kfree(buffer);
            return 0;
        }
    }

    if (n < 0) {
        kfree(buffer);
        return 0;
    }

    buffer[length] = '\0';
    *out_buffer = buffer;
    *out_length = length;
    return 1;
}

static int shell_load_text_argument_or_pipe(const char* path_arg,
                                            char** out_buffer,
                                            unsigned int* out_length,
                                            char* resolved_path,
                                            unsigned int resolved_size) {
    if (out_buffer == 0 || out_length == 0) {
        return 0;
    }

    *out_buffer = 0;
    *out_length = 0;

    if (path_arg != 0) {
        int fd;
        const char* path_to_open = path_arg;

        if (resolved_path != 0 && resolved_size > 0) {
            shell_resolve_path(path_arg, resolved_path, resolved_size);
            path_to_open = resolved_path;
        }

        fd = vfs_open_flags(path_to_open, VFS_O_RDONLY);
        if (fd < 0) {
            terminal_print("[error] Archivo no encontrado: ");
            terminal_print_line(path_to_open);
            return 0;
        }

        if (!shell_read_fd_fully(fd, out_buffer, out_length)) {
            vfs_close(fd);
            return 0;
        }

        vfs_close(fd);
        return 1;
    }

    if (!shell_has_pipe_input()) {
        return 0;
    }

    {
        char* buffer = 0;
        unsigned int capacity = 0;
        unsigned int i;

        if (!shell_buffer_reserve(&buffer, &capacity, shell_pipe_length + 1U)) {
            return 0;
        }

        for (i = 0; i < shell_pipe_length; i++) {
            buffer[i] = shell_pipe_buffer[i];
        }

        buffer[shell_pipe_length] = '\0';
        *out_buffer = buffer;
        *out_length = shell_pipe_length;
    }

    return 1;
}

static int shell_text_contains(const char* text, const char* needle) {
    unsigned int i;
    unsigned int j;

    if (!text || !needle) return 0;
    if (needle[0] == '\0') return 1;

    for (i = 0; text[i] != '\0'; i++) {
        for (j = 0; needle[j] != '\0' && text[i + j] == needle[j]; j++)
            ;
        if (needle[j] == '\0') return 1;
    }

    return 0;
}

static int shell_redir_find(const char* name) {
    for (int i = 0; i < SHELL_REDIR_MAX; i++) {
        if (shell_redir_files[i].used && str_equals(shell_redir_files[i].name, name)) {
            return i;
        }
    }

    return -1;
}

static int shell_redir_store(const char* name, const char* content, unsigned int length, int append) {
    char vfs_path[VFS_PATH_MAX];
    int  fd;
    unsigned int open_flags;

    if (name == 0 || name[0] == '\0' || content == 0) {
        return 0;
    }

    /* Resolve the output filename against cwd so "test.txt" becomes "/cwd/test.txt" */
    shell_resolve_path(name, vfs_path, sizeof(vfs_path));

    open_flags = VFS_O_WRONLY | VFS_O_CREAT;
    open_flags |= append ? VFS_O_APPEND : VFS_O_TRUNC;

    /* Open and write */
    fd = vfs_open_flags(vfs_path, open_flags);
    if (fd < 0) return 0;

    vfs_write(fd, (const unsigned char*)content, length);
    vfs_close(fd);
    return 1;
}

static int shell_read_text_source(const char* name, char* buffer, unsigned int buffer_size) {
    int index;
    unsigned int size;
    int read;

    if (name == 0 || buffer == 0 || buffer_size == 0) {
        return -1;
    }

    index = shell_redir_find(name);
    if (index >= 0) {
        size = shell_redir_files[index].length;
        if (size >= buffer_size) {
            size = buffer_size - 1;
        }

        for (unsigned int i = 0; i < size; i++) {
            buffer[i] = shell_redir_files[index].content[i];
        }

        buffer[size] = '\0';
        return (int)size;
    }

    size = fs_size(name);
    if (size == 0) {
        return -1;
    }

    if (size >= buffer_size) {
        size = buffer_size - 1;
    }

    read = fs_read_bytes(name, (unsigned char*)buffer, size);
    if (read < 0) {
        return -1;
    }

    buffer[read] = '\0';
    return read;
}

static void shell_apply_theme(const char* name, int print_feedback) {
    if (name == 0 || str_equals(name, "default")) {
        current_theme_name = "default";
        current_theme_banner_color = 0x0B;
        current_theme_hint_color = 0x07;
        terminal_set_color(0x0F);
        shell_input_set_theme(0x08, 0x0B, 0x0A, 0x70);
    } else if (str_equals(name, "matrix")) {
        current_theme_name = "matrix";
        current_theme_banner_color = 0x0A;
        current_theme_hint_color = 0x02;
        terminal_set_color(0x0A);
        shell_input_set_theme(0x02, 0x0A, 0x0A, 0x20);
    } else if (str_equals(name, "amber")) {
        current_theme_name = "amber";
        current_theme_banner_color = 0x0E;
        current_theme_hint_color = 0x06;
        terminal_set_color(0x0E);
        shell_input_set_theme(0x06, 0x0E, 0x06, 0x60);
    } else if (str_equals(name, "ice")) {
        current_theme_name = "ice";
        current_theme_banner_color = 0x0F;
        current_theme_hint_color = 0x09;
        terminal_set_color(0x0F);
        shell_input_set_theme(0x09, 0x0B, 0x0F, 0x1F);
    } else {
        terminal_print_line("Uso: theme [default|matrix|amber|ice]");
        return;
    }

    if (print_feedback) {
        terminal_print("Tema activo: ");
        terminal_print_line(current_theme_name);
    }
}

static int command_name_matches(const char* input, const char* name) {
    return str_equals_ignore_case(input, name);
}

static unsigned int parse_positive_or_default(const char* text, unsigned int fallback) {
    int parsed = parser_parse_integer(text, (int)fallback);

    if (parsed <= 0) {
        return fallback;
    }

    return (unsigned int)parsed;
}

static unsigned int milliseconds_to_ticks(unsigned int milliseconds) {
    unsigned int frequency = timer_get_frequency();
    unsigned int ticks;

    if (frequency == 0) {
        return 1;
    }

    ticks = (milliseconds * frequency + 999U) / 1000U;
    return ticks == 0 ? 1 : ticks;
}

static int parse_priority_value(const char* text, task_priority_t* out_priority) {
    int numeric_value;

    if (text == 0 || out_priority == 0) {
        return 0;
    }

    if (str_equals_ignore_case(text, "high") || str_equals(text, "0")) {
        *out_priority = TASK_PRIORITY_HIGH;
        return 1;
    }

    if (str_equals_ignore_case(text, "normal") || str_equals(text, "1")) {
        *out_priority = TASK_PRIORITY_NORMAL;
        return 1;
    }

    if (str_equals_ignore_case(text, "low") || str_equals(text, "2")) {
        *out_priority = TASK_PRIORITY_LOW;
        return 1;
    }

    numeric_value = parser_parse_integer(text, -1);
    if (numeric_value < 0 || numeric_value > 2) {
        return 0;
    }

    *out_priority = (task_priority_t)numeric_value;
    return 1;
}

static void print_joined_args(int argc, const char* argv[], int start_index) {
    for (int i = start_index; i < argc; i++) {
        if (i > start_index) {
            terminal_put_char(' ');
        }
        terminal_print(argv[i]);
    }
}

static void shell_print_job_started(int id, const char* name) {
    terminal_print("[job ");
    terminal_print_uint((unsigned int)id);
    terminal_print("] ");
    terminal_print(name);
    terminal_print_line(" iniciado");
}

static void count_task_step(void) {
    count_task_data_t* data = (count_task_data_t*)task_current_data();

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        terminal_print_line("^C");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    if (data->current >= data->limit) {
        terminal_print_line("");
        task_exit(0);
        return;
    }

    terminal_put_char('#');
    data->current++;
    syscall_sleep(data->delay_ticks);
}

static void sleep_task_step(void) {
    sleep_task_data_t* data = (sleep_task_data_t*)task_current_data();

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        terminal_print_line("sleep cancelado");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    if (!data->armed) {
        data->armed = 1;
        syscall_sleep(data->delay_ticks);
        return;
    }

    terminal_print_line("sleep completado");
    task_exit(0);
}

static void wait_task_step(void) {
    wait_task_data_t* data = (wait_task_data_t*)task_current_data();

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        terminal_print_line("wait cancelado");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    if (!data->armed) {
        data->armed = 1;
        terminal_print("Esperando evento ");
        terminal_print_uint((unsigned int)data->event_id);
        terminal_put_char('\n');
        task_wait_event(data->event_id);
        return;
    }

    terminal_print("Evento recibido: ");
    terminal_print_uint((unsigned int)data->event_id);
    terminal_put_char('\n');
    task_exit(0);
}

static void mq_send_wait_task_step(void) {
    mq_send_wait_task_data_t* data = (mq_send_wait_task_data_t*)task_current_data();
    unsigned int now;
    unsigned int remaining_ticks;
    int event_id;
    int rc;

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        terminal_print_line("mq send cancelado");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    rc = task_mq_send(data->queue_id, data->payload, data->payload_size);
    if (rc == 0) {
        terminal_print_line("mq send: ok");
        task_exit(0);
        return;
    }

    if (rc != MQ_E_FULL) {
        terminal_print_line("[error] no se pudo enviar a la cola MQ");
        task_exit(1);
        return;
    }

    if (!data->wait_started) {
        if (data->timeout_ticks == 0U) {
            terminal_print_line("[error] cola MQ llena");
            task_exit(1);
            return;
        }

        data->wait_started = 1;
        data->deadline_tick = timer_get_ticks() + data->timeout_ticks;
    }

    now = timer_get_ticks();
    if ((int)(now - data->deadline_tick) >= 0) {
        terminal_print_line("mq send: timeout");
        task_exit(1);
        return;
    }

    event_id = task_mq_write_event_id(data->queue_id);
    if (event_id < 0) {
        terminal_print_line("[error] no existe esa cola MQ");
        task_exit(1);
        return;
    }

    remaining_ticks = data->deadline_tick - now;
    task_wait_event_timeout(event_id, remaining_ticks);
}

static void mq_recv_wait_task_step(void) {
    mq_recv_wait_task_data_t* data = (mq_recv_wait_task_data_t*)task_current_data();
    char payload[MQ_MAX_MESSAGE_SIZE];
    unsigned int received_size = 0;
    unsigned int now;
    unsigned int remaining_ticks;
    int event_id;
    int rc;

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        terminal_print_line("mq recv cancelado");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    rc = task_mq_receive(data->queue_id, payload, sizeof(payload), &received_size);
    if (rc == 0) {
        payload[(received_size < sizeof(payload)) ? received_size : (sizeof(payload) - 1U)] = '\0';
        terminal_print("mq recv: ");
        terminal_print_line(payload);
        task_exit(0);
        return;
    }

    if (rc != MQ_E_EMPTY) {
        terminal_print_line("[error] no se pudo recibir de la cola MQ");
        task_exit(1);
        return;
    }

    if (!data->wait_started) {
        if (data->timeout_ticks == 0U) {
            terminal_print_line("mq recv: vacia");
            task_exit(1);
            return;
        }

        data->wait_started = 1;
        data->deadline_tick = timer_get_ticks() + data->timeout_ticks;
    }

    now = timer_get_ticks();
    if ((int)(now - data->deadline_tick) >= 0) {
        terminal_print_line("mq recv: timeout");
        task_exit(1);
        return;
    }

    event_id = task_mq_read_event_id(data->queue_id);
    if (event_id < 0) {
        terminal_print_line("[error] no existe esa cola MQ");
        task_exit(1);
        return;
    }

    remaining_ticks = data->deadline_tick - now;
    task_wait_event_timeout(event_id, remaining_ticks);
}

static void mq_delayed_send_task_step(void) {
    mq_delayed_send_task_data_t* data = (mq_delayed_send_task_data_t*)task_current_data();
    int rc;

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        task_clear_cancel();
        task_exit(1);
        return;
    }

    if (!data->armed) {
        data->armed = 1;
        syscall_sleep(data->delay_ticks);
        return;
    }

    rc = task_mq_send(data->queue_id, data->payload, data->payload_size);
    if (rc != 0) {
        terminal_print_line("[error] mq demo: fallo al enviar en diferido");
        task_exit(1);
        return;
    }

    task_exit(0);
}

static void mq_demo_cleanup(mq_demo_task_data_t* data) {
    if (data != 0 && data->queue_id >= 0) {
        task_mq_unlink(data->queue_id);
        data->queue_id = -1;
    }
}

static void mq_demo_task_step(void) {
    mq_demo_task_data_t* data = (mq_demo_task_data_t*)task_current_data();
    unsigned int now;
    unsigned int remaining_ticks;
    char payload[MQ_MAX_MESSAGE_SIZE];
    unsigned int received_size = 0;
    int event_id;
    int rc;

    if (data == 0) {
        task_exit(1);
        return;
    }

    if (task_cancel_requested()) {
        mq_demo_cleanup(data);
        terminal_print_line("mq demo cancelado");
        task_clear_cancel();
        task_exit(1);
        return;
    }

    if (data->phase == 0) {
        mq_delayed_send_task_data_t sender_data;
        int sender_id;
        static const char delayed_payload[] = "desbloqueado";

        data->queue_id = task_mq_create(1U, 32U);
        if (data->queue_id < 0) {
            terminal_print_line("[error] no se pudo crear la cola MQ para la demo");
            task_exit(1);
            return;
        }

        sender_data.queue_id = data->queue_id;
        sender_data.delay_ticks = milliseconds_to_ticks(40U);
        sender_data.payload_size = sizeof(delayed_payload);
        sender_data.armed = 0;
        for (unsigned int i = 0; i < sizeof(delayed_payload); i++) {
            sender_data.payload[i] = delayed_payload[i];
        }

        sender_id = task_spawn("mqtx", mq_delayed_send_task_step, &sender_data, sizeof(sender_data), 0);
        if (sender_id < 0) {
            mq_demo_cleanup(data);
            terminal_print_line("[error] mq demo: no se pudo crear el emisor diferido");
            task_exit(1);
            return;
        }

        data->phase = 1;
        data->recv_deadline_tick = timer_get_ticks() + milliseconds_to_ticks(200U);
    }

    if (data->phase == 1) {
        rc = task_mq_receive(data->queue_id, payload, sizeof(payload), &received_size);
        if (rc == 0) {
            payload[(received_size < sizeof(payload)) ? received_size : (sizeof(payload) - 1U)] = '\0';
            terminal_print("mqrecv unblock: ");
            terminal_print_line(payload);
            data->phase = 2;
        } else if (rc == MQ_E_EMPTY) {
            now = timer_get_ticks();
            if ((int)(now - data->recv_deadline_tick) >= 0) {
                mq_demo_cleanup(data);
                terminal_print_line("[error] mq demo: recv no se desbloqueo");
                task_exit(1);
                return;
            }

            event_id = task_mq_read_event_id(data->queue_id);
            if (event_id < 0) {
                mq_demo_cleanup(data);
                terminal_print_line("[error] mq demo: cola invalida en recv");
                task_exit(1);
                return;
            }

            remaining_ticks = data->recv_deadline_tick - now;
            task_wait_event_timeout(event_id, remaining_ticks);
            return;
        } else {
            mq_demo_cleanup(data);
            terminal_print_line("[error] mq demo: fallo al recibir");
            task_exit(1);
            return;
        }
    }

    if (data->phase == 2) {
        static const char occupied_payload[] = "ocupada";

        rc = task_mq_send(data->queue_id, occupied_payload, sizeof(occupied_payload));
        if (rc != 0) {
            mq_demo_cleanup(data);
            terminal_print_line("[error] mq demo: fallo al llenar la cola");
            task_exit(1);
            return;
        }

        data->phase = 3;
        data->send_deadline_tick = timer_get_ticks() + milliseconds_to_ticks(80U);
    }

    if (data->phase == 3) {
        static const char blocked_payload[] = "bloqueada";

        rc = task_mq_send(data->queue_id, blocked_payload, sizeof(blocked_payload));
        if (rc == 0) {
            mq_demo_cleanup(data);
            terminal_print_line("[error] mq demo: el timeout de send no se activo");
            task_exit(1);
            return;
        }

        if (rc != MQ_E_FULL) {
            mq_demo_cleanup(data);
            terminal_print_line("[error] mq demo: fallo inesperado en send");
            task_exit(1);
            return;
        }

        now = timer_get_ticks();
        if ((int)(now - data->send_deadline_tick) >= 0) {
            terminal_print_line("mq send timeout ok");
            data->phase = 4;
        } else {
            event_id = task_mq_write_event_id(data->queue_id);
            if (event_id < 0) {
                mq_demo_cleanup(data);
                terminal_print_line("[error] mq demo: cola invalida en send");
                task_exit(1);
                return;
            }

            remaining_ticks = data->send_deadline_tick - now;
            task_wait_event_timeout(event_id, remaining_ticks);
            return;
        }
    }

    if (data->phase == 4) {
        mq_demo_cleanup(data);
        terminal_print_line("mq demo ok");
        task_exit(0);
        return;
    }
}

static int cmd_help(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;

    terminal_print_line("Comandos disponibles:");
    for (int i = 0; i < command_count; i++) {
        terminal_print("  ");
        terminal_print(commands[i].name);
        terminal_print(" - ");
        terminal_print_line(commands[i].help);
    }

    return 1;
}

static int cmd_clear(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    terminal_clear();
    return 1;
}

static int cmd_about(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    terminal_print_line(LYTH_KERNEL_PRETTY_NAME);
    terminal_print_line("Kernel hobby en C + ASM");
    terminal_print_line("PIT, scheduler, heap, syscalls y FS en memoria");
    terminal_print("Build: ");
    terminal_print_line(LYTH_KERNEL_BUILD_FLAVOR);
    return 1;
}

static int cmd_echo(int argc, const char* argv[], int background) {
    (void)background;

    if (argc <= 1) {
        terminal_print_line("");
        return 1;
    }

    print_joined_args(argc, argv, 1);
    terminal_put_char('\n');
    return 1;
}

static int cmd_env(int argc, const char* argv[], int background) {
    int printed = 0;

    (void)argc;
    (void)argv;
    (void)background;

    for (int i = 0; i < SHELL_ENV_MAX; i++) {
        if (!shell_env[i].used) {
            continue;
        }

        terminal_print(shell_env[i].name);
        terminal_print("=");
        terminal_print_line(shell_env[i].value);
        printed = 1;
    }

    if (!printed) {
        terminal_print_line("Sin variables de entorno");
    }

    return 1;
}

static int cmd_set(int argc, const char* argv[], int background) {
    char value[SHELL_ENV_VALUE_MAX];

    (void)background;

    if (argc < 3) {
        terminal_print_line("Uso: set <NOMBRE> <VALOR>");
        return 1;
    }

    shell_join_args_to_buffer(argc, argv, 2, value, sizeof(value));
    if (!shell_env_set(argv[1], value)) {
        terminal_print_line("No se pudo definir la variable");
        return 1;
    }

    terminal_print(argv[1]);
    terminal_print("=");
    terminal_print_line(value);
    shell_write_user_profile();
    return 1;
}

static int cmd_unset(int argc, const char* argv[], int background) {
    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: unset <NOMBRE>");
        return 1;
    }

    if (!shell_env_unset(argv[1])) {
        terminal_print_line("Variable no encontrada");
        return 1;
    }

    terminal_print("Variable eliminada: ");
    terminal_print_line(argv[1]);
    shell_write_user_profile();
    return 1;
}

static int cmd_keymap(int argc, const char* argv[], int background) {
    keyboard_layout_t layout;

    (void)background;

    if (argc < 2) {
        terminal_print("Layout actual: ");
        terminal_print_line(keyboard_layout_name(keyboard_get_layout()));
        return 1;
    }

    if (str_equals_ignore_case(argv[1], "us")) {
        layout = KEYBOARD_LAYOUT_US;
    } else if (str_equals_ignore_case(argv[1], "es")) {
        layout = KEYBOARD_LAYOUT_ES;
    } else {
        terminal_print_line("Uso: keymap [us|es]");
        return 1;
    }

    keyboard_set_layout(layout);
    terminal_print("Layout activo: ");
    terminal_print_line(keyboard_layout_name(layout));
    shell_write_user_profile();
    return 1;
}

static int cmd_color(int argc, const char* argv[], int background) {
    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: color green|red|white|blue");
        return 1;
    }

    if (str_equals(argv[1], "white")) {
        terminal_set_color(0x0F);
        terminal_print_line("Color cambiado a white");
        return 1;
    }

    if (str_equals(argv[1], "green")) {
        terminal_set_color(0x0A);
        terminal_print_line("Color cambiado a green");
        return 1;
    }

    if (str_equals(argv[1], "red")) {
        terminal_set_color(0x0C);
        terminal_print_line("Color cambiado a red");
        return 1;
    }

    if (str_equals(argv[1], "blue")) {
        terminal_set_color(0x09);
        terminal_print_line("Color cambiado a blue");
        return 1;
    }

    terminal_print_line("Uso: color green|red|white|blue");
    return 1;
}

static int cmd_theme(int argc, const char* argv[], int background) {
    (void)background;

    if (argc < 2) {
        terminal_print("Tema activo: ");
        terminal_print_line(current_theme_name);
        terminal_print_line("Disponibles: default, matrix, amber, ice");
        return 1;
    }

    shell_apply_theme(argv[1], 1);
    if (str_equals(argv[1], "default") ||
        str_equals(argv[1], "matrix") ||
        str_equals(argv[1], "amber") ||
        str_equals(argv[1], "ice")) {
        shell_write_user_profile();
    }
    return 1;
}

static int cmd_history(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    shell_input_print_history();
    return 1;
}

static int cmd_count(int argc, const char* argv[], int background) {
    count_task_data_t data;
    int id;

    data.current = 0;
    data.limit = argc >= 2 ? (int)parse_positive_or_default(argv[1], 20) : 20;
    data.delay_ticks = milliseconds_to_ticks(100);

    id = task_spawn("count", count_task_step, &data, sizeof(data), background ? 0 : 1);
    if (id < 0) {
        terminal_print_line("No se pudo crear la tarea count");
        return 1;
    }

    if (background) {
        shell_print_job_started(id, "count");
        return 1;
    }

    terminal_print_line("Ejecutando count. Pulsa Ctrl+C para cancelarlo.");
    return 0;
}

static int cmd_sleep(int argc, const char* argv[], int background) {
    sleep_task_data_t data;
    unsigned int milliseconds;
    int id;

    if (argc < 2) {
        terminal_print_line("Uso: sleep <ms> [&]");
        return 1;
    }

    milliseconds = parse_positive_or_default(argv[1], 1000);
    data.delay_ticks = milliseconds_to_ticks(milliseconds);
    data.armed = 0;

    id = task_spawn("sleep", sleep_task_step, &data, sizeof(data), background ? 0 : 1);
    if (id < 0) {
        terminal_print_line("No se pudo crear la tarea sleep");
        return 1;
    }

    if (background) {
        shell_print_job_started(id, "sleep");
        return 1;
    }

    terminal_print("Sleep por ");
    terminal_print_uint(milliseconds);
    terminal_print_line(" ms");
    return 0;
}

static int cmd_uptime(int argc, const char* argv[], int background) {
    unsigned int ticks;
    unsigned int ms;
    unsigned int h, m, s;
    unsigned int idle_ticks = 0;
    unsigned int ctx_sw = 0;

    (void)argc;
    (void)argv;
    (void)background;

    ticks = timer_get_ticks();
    ms    = timer_ticks_to_ms(ticks);
    h     = ms / 3600000U;
    m     = (ms % 3600000U) / 60000U;
    s     = (ms % 60000U) / 1000U;

    terminal_print("Uptime: ");
    terminal_print_uint(h);
    terminal_print("h ");
    terminal_print_uint(m);
    terminal_print("m ");
    terminal_print_uint(s);
    terminal_print("s  (");
    terminal_print_uint(ms);
    terminal_print(" ms, ");
    terminal_print_uint(ticks);
    terminal_print(" ticks)\n");

    task_idle_stats(&idle_ticks, &ctx_sw);
    terminal_print("Idle ticks: ");
    terminal_print_uint(idle_ticks);
    terminal_print(" | ctx switches: ");
    terminal_print_uint(ctx_sw);
    terminal_print("\n");

    if (ticks > 0U) {
        terminal_print("Idle %: ");
        terminal_print_uint((idle_ticks * 100U) / ticks);
        terminal_print("%\n");
    }
    return 1;
}

/* ---- ulimit ---- */
static int cmd_ulimit(int argc, const char* argv[], int background) {
    unsigned int soft = 0U, hard = 0U;
    int show_soft = 1, show_hard = 0;
    int i;

    (void)background;

    task_get_fd_rlimit(&soft, &hard);

    /* Parse flags */
    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            const char* opt = argv[i] + 1;
            if (opt[0] == 'H' && opt[1] == '\0') {
                show_hard = 1; show_soft = 0;
            } else if (opt[0] == 'S' && opt[1] == '\0') {
                show_soft = 1; show_hard = 0;
            } else if (opt[0] == 'n' && opt[1] == '\0') {
                /* -n <value> : set soft nofile limit */
                if (i + 1 >= argc) {
                    terminal_print("ulimit: -n requiere un valor\n");
                    return 0;
                }
                i++;
                {
                    unsigned int new_soft = (unsigned int)parser_parse_integer(argv[i], -1);
                    rlimit_t rl;
                    rl.rlim_cur = new_soft;
                    rl.rlim_max = hard;   /* keep current hard */
                    if (task_set_fd_rlimit(rl.rlim_cur, rl.rlim_max) != 0) {
                        terminal_print("ulimit: valor invalido o supera el limite maximo (");
                        terminal_print_uint(hard);
                        terminal_print(")\n");
                        return 0;
                    }
                    /* re-read to confirm */
                    task_get_fd_rlimit(&soft, &hard);
                }
                return 1;
            } else if (opt[0] == 'H' && opt[1] == 'n') {
                /* -Hn : show hard nofile */
                show_hard = 1; show_soft = 0;
            } else {
                terminal_print("ulimit: opcion desconocida: ");
                terminal_print(argv[i]);
                terminal_print("\nUso: ulimit [-S|-H] [-n <valor>]\n");
                return 0;
            }
        } else {
            terminal_print("ulimit: argumento inesperado: ");
            terminal_print(argv[i]);
            terminal_print("\n");
            return 0;
        }
    }

    /* Display */
    if (argc == 1) {
        /* No args: show both */
        terminal_print("RLIMIT_NOFILE  soft=");
        terminal_print_uint(soft);
        terminal_print("  hard=");
        terminal_print_uint(hard);
        terminal_print("  open=");
        terminal_print_uint((unsigned int)task_current_open_fd_count());
        terminal_print("\n");
    } else if (show_hard) {
        terminal_print_uint(hard);
        terminal_print("\n");
    } else if (show_soft) {
        terminal_print_uint(soft);
        terminal_print("\n");
    }
    return 1;
}

static void date_print_padded2(unsigned int v) {
    if (v < 10U) { terminal_print("0"); }
    terminal_print_uint(v);
}

static int cmd_date(int argc, const char* argv[], int background) {
    rtc_time_t t;
    unsigned int epoch;

    (void)argc;
    (void)argv;
    (void)background;

    rtc_read(&t);
    epoch = rtc_get_wall_epoch();

    terminal_print("Fecha: ");
    terminal_print_uint(t.year);
    terminal_print("-");
    date_print_padded2(t.month);
    terminal_print("-");
    date_print_padded2(t.day);
    terminal_print("  ");
    date_print_padded2(t.hour);
    terminal_print(":");
    date_print_padded2(t.min);
    terminal_print(":");
    date_print_padded2(t.sec);
    terminal_print("\n");

    terminal_print("Epoch:  ");
    terminal_print_uint(epoch);
    terminal_print("\n");

    terminal_print("Monot:  ");
    terminal_print_uint(timer_get_monotonic_us());
    terminal_print(" us\n");

    return 1;
}

static int cmd_ps(int argc, const char* argv[], int background) {
    task_snapshot_t snapshots[16];
    int count;

    (void)argc;
    (void)argv;
    (void)background;

    count = task_list(snapshots, 16);
    if (count == 0) {
        terminal_print_line("No hay tareas activas");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        terminal_print("PID=");
        terminal_print_uint((unsigned int)snapshots[i].id);
        terminal_print(" PPID=");
        terminal_print_uint((unsigned int)snapshots[i].parent_id);
        terminal_print(" ");
        terminal_print(snapshots[i].name);
        terminal_print(" ");
        terminal_print(task_state_name(snapshots[i].state));
        terminal_print(" PRIO=");
        terminal_print(task_priority_name(snapshots[i].priority));
        if (snapshots[i].state == TASK_STATE_BLOCKED) {
            terminal_print(" EVT=");
            terminal_print_uint((unsigned int)snapshots[i].blocked_event_id);
        }
        terminal_print(snapshots[i].foreground ? " FG" : " BG");
        if (snapshots[i].state == TASK_STATE_ZOMBIE) {
            terminal_print(" EC=");
            terminal_print_uint((unsigned int)snapshots[i].exit_code);
        }
        if (snapshots[i].cancel_requested) {
            terminal_print(" cancel");
        }
        terminal_put_char('\n');
    }

    return 1;
}

static int cmd_kill(int argc, const char* argv[], int background) {
    unsigned int id;
    int signum = LYTH_SIGTERM;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: kill <id> [signum]");
        return 1;
    }

    if (argc >= 3) {
        signum = parser_parse_integer(argv[2], LYTH_SIGTERM);
        if (signum <= 0 || signum > LYTH_SIGNAL_MAX) {
            terminal_print_line("Signum invalido");
            return 1;
        }
    }

    id = parse_positive_or_default(argv[1], 0);
    if (id == 0 || !task_send_signal((int)id, signum)) {
        terminal_print_line("No existe esa tarea");
        return 1;
    }

    terminal_print("Señal enviada a PID ");
    terminal_print_uint(id);
    terminal_print(" SIG=");
    terminal_print_uint((unsigned int)signum);
    terminal_put_char('\n');
    return 1;
}

static int cmd_nice(int argc, const char* argv[], int background) {
    unsigned int id;
    task_priority_t priority;

    (void)background;

    if (argc < 3) {
        terminal_print_line("Uso: nice <id> <high|normal|low>");
        return 1;
    }

    id = parse_positive_or_default(argv[1], 0);
    if (id == 0 || !parse_priority_value(argv[2], &priority)) {
        terminal_print_line("Prioridad invalida");
        return 1;
    }

    if (!task_set_priority((int)id, priority)) {
        terminal_print_line("No existe esa tarea");
        return 1;
    }

    terminal_print("Prioridad actualizada: ");
    terminal_print_uint(id);
    terminal_print(" -> ");
    terminal_print_line(task_priority_name(priority));
    return 1;
}

/* ---- cmd_renice ---- */
static int cmd_renice(int argc, const char* argv[], int background) {
    unsigned int id;
    task_priority_t priority;
    (void)background;
    if (argc < 3) { terminal_print_line("Uso: renice <high|normal|low> <id>"); return 1; }
    if (!parse_priority_value(argv[1], &priority)) {
        terminal_print("[error] Prioridad desconocida: "); terminal_print_line(argv[1]);
        return 1;
    }
    id = parse_positive_or_default(argv[2], 0);
    if (id == 0 || !task_set_priority((int)id, priority)) {
        terminal_print_line("[error] No existe esa tarea");
        return 1;
    }
    terminal_print("PID "); terminal_print_uint(id);
    terminal_print(" -> "); terminal_print_line(task_priority_name(priority));
    return 1;
}

/* ---- cmd_killall ---- */
static int cmd_killall(int argc, const char* argv[], int background) {
    task_snapshot_t snaps[32];
    int count, i, killed = 0;
    int signum = LYTH_SIGTERM;
    const char* target_name;
    (void)background;
    if (argc < 2) { terminal_print_line("Uso: killall <nombre> [signum]"); return 1; }
    target_name = argv[1];
    if (argc >= 3) {
        signum = parser_parse_integer(argv[2], LYTH_SIGTERM);
        if (signum <= 0 || signum > LYTH_SIGNAL_MAX) {
            terminal_print_line("[error] Signum invalido"); return 1;
        }
    }
    count = task_list(snaps, 32);
    for (i = 0; i < count; i++) {
        /* skip shell itself */
        if (snaps[i].id == task_current_id()) continue;
        if (!str_equals(snaps[i].name, target_name)) continue;
        if (task_send_signal(snaps[i].id, signum)) {
            terminal_print("Señal enviada a PID ");
            terminal_print_uint((unsigned int)snaps[i].id);
            terminal_put_char('\n');
            killed++;
        }
    }
    if (killed == 0) { terminal_print(target_name); terminal_print_line(": no encontrado"); }
    return 1;
}

/* ---- cmd_pidof ---- */
static int cmd_pidof(int argc, const char* argv[], int background) {
    task_snapshot_t snaps[32];
    int count, i, found = 0;
    (void)background;
    if (argc < 2) { terminal_print_line("Uso: pidof <nombre>"); return 1; }
    count = task_list(snaps, 32);
    for (i = 0; i < count; i++) {
        if (!str_equals(snaps[i].name, argv[1])) continue;
        if (found) terminal_put_char(' ');
        terminal_print_uint((unsigned int)snaps[i].id);
        found++;
    }
    if (found) terminal_put_char('\n');
    else { terminal_print(argv[1]); terminal_print_line(": no encontrado"); }
    return 1;
}

/* ---- cmd_fg ---- */
static int cmd_fg(int argc, const char* argv[], int background) {
    task_snapshot_t snaps[32];
    int count, i;
    int target_id = -1;
    (void)background;

    if (argc >= 2) {
        target_id = (int)parse_positive_or_default(argv[1], 0);
        if (target_id == 0) { terminal_print_line("[error] PID invalido"); return 1; }
    } else {
        /* Pick the most recently spawned non-shell BG task */
        count = task_list(snaps, 32);
        for (i = count - 1; i >= 0; i--) {
            if (snaps[i].id == task_current_id()) continue;
            if (snaps[i].foreground) continue;
            if (snaps[i].state == TASK_STATE_FINISHED ||
                snaps[i].state == TASK_STATE_ZOMBIE   ||
                snaps[i].state == TASK_STATE_CANCELLED) continue;
            target_id = snaps[i].id;
            break;
        }
        if (target_id < 0) { terminal_print_line("No hay tareas en background"); return 1; }
    }

    if (!task_set_foreground(target_id)) {
        terminal_print_line("[error] No existe o ya terminó esa tarea");
        return 1;
    }
    terminal_print("Foreground -> PID "); terminal_print_uint((unsigned int)target_id);
    terminal_put_char('\n');
    task_wait_id(target_id);
    return 1;
}

/* ---- cmd_bg ---- */
static int cmd_bg(int argc, const char* argv[], int background) {
    task_snapshot_t snaps[32];
    int count, i, found = 0;
    (void)background;

    if (argc >= 2) {
        /* Move specified FG task to BG — get the current FG and check it matches */
        int req_id = (int)parse_positive_or_default(argv[1], 0);
        if (req_id == 0) { terminal_print_line("[error] PID invalido"); return 1; }
        if (task_foreground_task_id() == req_id) {
            /* Demote it: mark as background by bringing no task to FG */
            task_set_foreground(-1);   /* -1 = just clear current FG */
            terminal_print("PID "); terminal_print_uint((unsigned int)req_id);
            terminal_print_line(" enviado a background");
        } else {
            terminal_print("[error] PID ");
            terminal_print_uint((unsigned int)req_id);
            terminal_print_line(" no es la tarea foreground actual");
        }
        return 1;
    }

    /* No arg: list background tasks */
    count = task_list(snaps, 32);
    for (i = 0; i < count; i++) {
        if (snaps[i].foreground) continue;
        if (snaps[i].id == task_current_id()) continue;
        if (snaps[i].state == TASK_STATE_FINISHED ||
            snaps[i].state == TASK_STATE_ZOMBIE   ||
            snaps[i].state == TASK_STATE_CANCELLED) continue;
        terminal_print("[");
        terminal_print_uint((unsigned int)snaps[i].id);
        terminal_print("] ");
        terminal_print(snaps[i].name);
        terminal_print(" ");
        terminal_print_line(task_state_name(snaps[i].state));
        found++;
    }
    if (!found) terminal_print_line("No hay tareas en background");
    return 1;
}

/* ---- cmd_time ---- */
static int cmd_time(int argc, const char* argv[], int background) {
    unsigned int t0, t1, elapsed_ms;
    int i, sub_argc;
    const char* sub_argv[16];
    int found = 0;
    (void)background;

    if (argc < 2) { terminal_print_line("Uso: time <comando> [args...]"); return 1; }

    /* Build sub-command argv */
    sub_argc = argc - 1;
    if (sub_argc > 16) sub_argc = 16;
    for (i = 0; i < sub_argc; i++) sub_argv[i] = argv[i + 1];

    /* Find and run the sub-command */
    t0 = timer_get_uptime_ms();
    for (i = 0; i < command_count; i++) {
        if (str_equals(commands[i].name, sub_argv[0])) {
            commands[i].fn(sub_argc, sub_argv, 0);
            found = 1;
            break;
        }
    }
    t1 = timer_get_uptime_ms();

    if (!found) {
        terminal_print("[error] Comando no encontrado: "); terminal_print_line(sub_argv[0]);
        return 1;
    }

    elapsed_ms = (t1 >= t0) ? (t1 - t0) : 0;
    terminal_print("\nreal\t");
    terminal_print_uint(elapsed_ms / 1000U);
    terminal_put_char('.');
    {
        unsigned int ms = elapsed_ms % 1000U;
        /* 3-digit zero-padded ms */
        terminal_put_char((char)('0' + ms / 100U));
        terminal_put_char((char)('0' + (ms % 100U) / 10U));
        terminal_put_char((char)('0' + ms % 10U));
    }
    terminal_print_line("s");
    return 1;
}

/* ---- cmd_stackbomb ---- */
static int cmd_stackbomb(int argc, const char* argv[], int background) {
    int id;
    (void)argc;
    (void)argv;
    (void)background;

    id = usermode_spawn_stackbomb(0);
    if (id < 0) {
        terminal_print_line("No se pudo crear la prueba stackbomb");
        return 1;
    }

    task_set_priority(id, TASK_PRIORITY_HIGH);

    shell_print_job_started(id, "stackbomb");
    terminal_print_line("stackbomb: prueba lanzada en background");
    return 1;
}

/* ---- cmd_stackok ---- */
static int cmd_stackok(int argc, const char* argv[], int background) {
    int id;
    (void)argc;
    (void)argv;
    (void)background;

    id = usermode_spawn_stackok(0);
    if (id < 0) {
        terminal_print_line("No se pudo crear la prueba stackok");
        return 1;
    }

    task_set_priority(id, TASK_PRIORITY_HIGH);

    shell_print_job_started(id, "stackok");
    terminal_print_line("stackok: prueba lanzada en background");
    return 1;
}

/* ---- cmd_shm ---- */
static int cmd_shm(int argc, const char* argv[], int background) {
    shm_segment_info_t infos[SHM_MAX_SEGMENTS];
    int count;
    (void)background;

    if (argc < 2 || str_equals(argv[1], "list")) {
        count = task_shm_list(infos, SHM_MAX_SEGMENTS);
        if (count <= 0) {
            terminal_print_line("No hay segmentos SHM");
            return 1;
        }

        terminal_print_line("ID   SIZE   REFS   STATE");
        for (int i = 0; i < count; i++) {
            terminal_print_uint((unsigned int)infos[i].id);
            terminal_print("   ");
            terminal_print_uint(infos[i].size);
            terminal_print("   ");
            terminal_print_uint(infos[i].ref_count);
            terminal_print("   ");
            terminal_print_line(infos[i].marked_for_delete ? "pending-delete" : "active");
        }
        return 1;
    }

    if (str_equals(argv[1], "create")) {
        unsigned int size;
        int segment_id;

        if (argc < 3) {
            terminal_print_line("uso: shm create <bytes>");
            return 1;
        }

        size = parse_positive_or_default(argv[2], 0);
        if (size == 0U) {
            terminal_print_line("[error] tamano invalido");
            return 1;
        }

        segment_id = task_shm_create(size);
        if (segment_id < 0) {
            terminal_print_line("[error] no se pudo crear el segmento SHM");
            return 1;
        }

        terminal_print("SHM creado: id=");
        terminal_print_uint((unsigned int)segment_id);
        terminal_print(" size=");
        terminal_print_uint(size);
        terminal_put_char('\n');
        return 1;
    }

    if (str_equals(argv[1], "unlink")) {
        int segment_id;

        if (argc < 3) {
            terminal_print_line("uso: shm unlink <id>");
            return 1;
        }

        segment_id = (int)parse_positive_or_default(argv[2], 0);
        if (segment_id <= 0) {
            terminal_print_line("[error] id invalido");
            return 1;
        }

        if (task_shm_unlink(segment_id) != 0) {
            terminal_print_line("[error] no existe ese segmento SHM");
            return 1;
        }

        terminal_print("SHM marcado para liberar: id=");
        terminal_print_uint((unsigned int)segment_id);
        terminal_put_char('\n');
        return 1;
    }

    terminal_print_line("uso: shm [list|create <bytes>|unlink <id>]");
    return 1;
}

/* ---- cmd_shmdemo ---- */
static int cmd_shmdemo(int argc, const char* argv[], int background) {
    unsigned int requested_value = 65U;
    unsigned char value;
    int segment_id;
    int writer_id;
    int reader_id;
    (void)background;

    if (argc >= 2) {
        requested_value = parse_positive_or_default(argv[1], 65U);
    }
    if (requested_value > 255U) {
        terminal_print_line("[error] byte invalido, debe estar entre 0 y 255");
        return 1;
    }
    value = (unsigned char)requested_value;

    segment_id = task_shm_create(1U);
    if (segment_id < 0) {
        terminal_print_line("[error] no se pudo crear el segmento SHM para la demo");
        return 1;
    }

    terminal_print("shmdemo: segmento creado id=");
    terminal_print_uint((unsigned int)segment_id);
    terminal_print(" valor=");
    terminal_print_uint((unsigned int)value);
    terminal_put_char('\n');

    writer_id = usermode_spawn_shm_writer(segment_id, value, 0);
    if (writer_id < 0) {
        terminal_print_line("[error] no se pudo lanzar shmwrite");
        task_shm_unlink(segment_id);
        return 1;
    }

    task_set_priority(writer_id, TASK_PRIORITY_HIGH);
    shell_print_job_started(writer_id, "shmwrite");
    task_wait_id(writer_id);

    reader_id = usermode_spawn_shm_reader(segment_id, value, 0);
    if (reader_id < 0) {
        terminal_print_line("[error] no se pudo lanzar shmread");
        task_shm_unlink(segment_id);
        return 1;
    }

    task_set_priority(reader_id, TASK_PRIORITY_HIGH);
    shell_print_job_started(reader_id, "shmread");
    task_wait_id(reader_id);

    if (task_shm_unlink(segment_id) != 0) {
        terminal_print_line("[warn] no se pudo liberar el segmento SHM");
        return 1;
    }

    terminal_print_line("shmdemo: segmento liberado");
    return 1;
}

/* ---- cmd_mq ---- */
static int cmd_mq(int argc, const char* argv[], int background) {
    mqueue_info_t infos[MQ_MAX_QUEUES];
    int count;

    if (argc < 2 || str_equals(argv[1], "list")) {
        count = task_mq_list(infos, MQ_MAX_QUEUES);
        if (count <= 0) {
            terminal_print_line("No hay colas MQ");
            return 1;
        }

        terminal_print_line("ID   DEPTH   MSGSZ   COUNT");
        for (int i = 0; i < count; i++) {
            terminal_print_uint((unsigned int)infos[i].id);
            terminal_print("   ");
            terminal_print_uint(infos[i].max_messages);
            terminal_print("   ");
            terminal_print_uint(infos[i].msg_size);
            terminal_print("   ");
            terminal_print_uint(infos[i].count);
            terminal_put_char('\n');
        }
        return 1;
    }

    if (str_equals(argv[1], "create")) {
        unsigned int depth;
        unsigned int msg_size;
        int queue_id;

        if (argc < 4) {
            terminal_print_line("uso: mq create <depth> <msg_size>");
            return 1;
        }

        depth = parse_positive_or_default(argv[2], 0U);
        msg_size = parse_positive_or_default(argv[3], 0U);
        queue_id = task_mq_create(depth, msg_size);
        if (queue_id < 0) {
            terminal_print_line("[error] no se pudo crear la cola MQ");
            return 1;
        }

        terminal_print("MQ creada: id=");
        terminal_print_uint((unsigned int)queue_id);
        terminal_put_char('\n');
        return 1;
    }

    if (str_equals(argv[1], "open")) {
        int queue_id;
        unsigned int open_flags = 0U;
        int fd;

        if (argc < 4) {
            terminal_print_line("uso: mq open <id> <r|w|rw> [nonblock]");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        if (str_equals(argv[3], "r")) {
            open_flags = VFS_O_RDONLY;
        } else if (str_equals(argv[3], "w")) {
            open_flags = VFS_O_WRONLY;
        } else if (str_equals(argv[3], "rw")) {
            open_flags = VFS_O_RDWR;
        } else {
            terminal_print_line("[error] modo invalido, usa r, w o rw");
            return 1;
        }

        if (argc >= 5 && str_equals(argv[4], "nonblock")) {
            open_flags |= VFS_O_NONBLOCK;
        }

        fd = task_mq_open(queue_id, open_flags);
        if (fd < 0) {
            terminal_print_line("[error] no se pudo abrir la cola MQ como FD");
            return 1;
        }

        terminal_print("mq fd=");
        terminal_print_uint((unsigned int)fd);
        terminal_put_char('\n');
        return 1;
    }

    if (str_equals(argv[1], "send")) {
        int queue_id;
        char payload[SHELL_PIPE_MAX];
        int rc;

        if (argc < 4) {
            terminal_print_line("uso: mq send <id> <texto>");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        shell_join_args_to_buffer(argc, argv, 3, payload, sizeof(payload));
        rc = task_mq_send(queue_id, payload, str_length(payload) + 1U);
        if (rc == MQ_E_FULL) {
            terminal_print_line("[error] cola MQ llena");
            return 1;
        }
        if (rc != 0) {
            terminal_print_line("[error] no se pudo enviar a la cola MQ");
            return 1;
        }

        terminal_print_line("mq send: ok");
        return 1;
    }

    if (str_equals(argv[1], "sendwait")) {
        mq_send_wait_task_data_t data;
        int timeout_ms;
        int queue_id;
        int id;

        if (argc < 5) {
            terminal_print_line("uso: mq sendwait <id> <timeout_ms> <texto>");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        timeout_ms = parser_parse_integer(argv[3], 0);
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }

        shell_join_args_to_buffer(argc, argv, 4, data.payload, sizeof(data.payload));
        data.queue_id = queue_id;
        data.timeout_ticks = timeout_ms <= 0 ? 0U : milliseconds_to_ticks((unsigned int)timeout_ms);
        data.deadline_tick = 0U;
        data.payload_size = str_length(data.payload) + 1U;
        data.wait_started = 0;

        id = task_spawn("mqsend", mq_send_wait_task_step, &data, sizeof(data), background ? 0 : 1);
        if (id < 0) {
            terminal_print_line("No se pudo crear la tarea mqsend");
            return 1;
        }

        if (background) {
            shell_print_job_started(id, "mqsend");
            return 1;
        }

        terminal_print("mq sendwait por ");
        terminal_print_uint((unsigned int)timeout_ms);
        terminal_print_line(" ms");
        return 0;
    }

    if (str_equals(argv[1], "recv")) {
        int queue_id;
        char payload[MQ_MAX_MESSAGE_SIZE];
        unsigned int received_size = 0;
        int rc;

        if (argc < 3) {
            terminal_print_line("uso: mq recv <id>");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        rc = task_mq_receive(queue_id, payload, sizeof(payload), &received_size);
        if (rc == MQ_E_EMPTY) {
            terminal_print_line("mq recv: vacia");
            return 1;
        }
        if (rc != 0) {
            terminal_print_line("[error] no se pudo recibir de la cola MQ");
            return 1;
        }

        payload[(received_size < sizeof(payload)) ? received_size : (sizeof(payload) - 1U)] = '\0';
        terminal_print("mq recv: ");
        terminal_print_line(payload);
        return 1;
    }

    if (str_equals(argv[1], "recvwait")) {
        mq_recv_wait_task_data_t data;
        int timeout_ms;
        int queue_id;
        int id;

        if (argc < 4) {
            terminal_print_line("uso: mq recvwait <id> <timeout_ms>");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        timeout_ms = parser_parse_integer(argv[3], 0);
        if (timeout_ms < 0) {
            timeout_ms = 0;
        }

        data.queue_id = queue_id;
        data.timeout_ticks = timeout_ms <= 0 ? 0U : milliseconds_to_ticks((unsigned int)timeout_ms);
        data.deadline_tick = 0U;
        data.wait_started = 0;

        id = task_spawn("mqrecv", mq_recv_wait_task_step, &data, sizeof(data), background ? 0 : 1);
        if (id < 0) {
            terminal_print_line("No se pudo crear la tarea mqrecv");
            return 1;
        }

        if (background) {
            shell_print_job_started(id, "mqrecv");
            return 1;
        }

        terminal_print("mq recvwait por ");
        terminal_print_uint((unsigned int)timeout_ms);
        terminal_print_line(" ms");
        return 0;
    }

    if (str_equals(argv[1], "unlink")) {
        int queue_id;

        if (argc < 3) {
            terminal_print_line("uso: mq unlink <id>");
            return 1;
        }

        queue_id = (int)parse_positive_or_default(argv[2], 0U);
        if (task_mq_unlink(queue_id) != 0) {
            terminal_print_line("[error] no existe esa cola MQ");
            return 1;
        }

        terminal_print_line("mq unlink: ok");
        return 1;
    }

    if (str_equals(argv[1], "demo")) {
        syscall_pollfd_t pfds[2];
        unsigned int read_mask;
        unsigned int write_mask;
        char buffer[MQ_MAX_MESSAGE_SIZE];
        unsigned int nfds;
        unsigned int send_deadline_tick;
        int poll_ready;
        int queue_id;
        int read_fd;
        int write_fd;
        static const char occupied_payload[] = "ocupada";
        static const char blocked_payload[] = "bloqueada";
        static const char poll_payload[] = "poll-msg";

        queue_id = task_mq_create(1U, 32U);
        if (queue_id < 0) {
            terminal_print_line("[error] no se pudo crear la cola MQ para la demo");
            return 1;
        }

        read_fd = task_mq_open(queue_id, VFS_O_RDONLY | VFS_O_NONBLOCK);
        write_fd = task_mq_open(queue_id, VFS_O_WRONLY | VFS_O_NONBLOCK);
        if (read_fd < 0 || write_fd < 0) {
            if (read_fd >= 0) {
                vfs_close(read_fd);
            }
            if (write_fd >= 0) {
                vfs_close(write_fd);
            }
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: no se pudo abrir la cola como FD");
            return 1;
        }

        pfds[0].fd = read_fd;
        pfds[0].events = SYSCALL_POLLIN;
        pfds[0].revents = 0U;
        pfds[1].fd = write_fd;
        pfds[1].events = SYSCALL_POLLOUT;
        pfds[1].revents = 0U;

        poll_ready = syscall_poll(pfds, 2U, 0U);
        if (poll_ready != 1 || pfds[0].revents != 0U || (pfds[1].revents & SYSCALL_POLLOUT) == 0U) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: poll inicial incorrecto");
            return 1;
        }
        terminal_print_line("mq poll write ok");

        if (vfs_write(write_fd, (const unsigned char*)poll_payload, sizeof(poll_payload)) != (int)sizeof(poll_payload)) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: fallo al escribir por FD");
            return 1;
        }

        pfds[0].revents = 0U;
        pfds[1].revents = 0U;
        poll_ready = syscall_poll(pfds, 2U, 0U);
        if (poll_ready != 1 || (pfds[0].revents & SYSCALL_POLLIN) == 0U || pfds[1].revents != 0U) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: poll tras write incorrecto");
            return 1;
        }

        read_mask = (1U << (unsigned int)read_fd);
        write_mask = (1U << (unsigned int)write_fd);
        nfds = (unsigned int)((read_fd > write_fd ? read_fd : write_fd) + 1);
        poll_ready = syscall_select(nfds, &read_mask, &write_mask, 0U);
        if (poll_ready != 1 || read_mask != (1U << (unsigned int)read_fd) || write_mask != 0U) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: select incorrecto");
            return 1;
        }
        terminal_print_line("mq select ok");

        if (vfs_read(read_fd, (unsigned char*)buffer, sizeof(buffer)) <= 0) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: fallo al leer por FD");
            return 1;
        }

        if (task_mq_send(queue_id, occupied_payload, sizeof(occupied_payload)) != 0) {
            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            terminal_print_line("[error] mq demo: fallo al llenar la cola");
            return 1;
        }

        send_deadline_tick = timer_get_ticks() + milliseconds_to_ticks(80U);
        for (;;) {
            int rc = task_mq_send(queue_id, blocked_payload, sizeof(blocked_payload));
            if (rc == MQ_E_FULL) {
                if ((int)(timer_get_ticks() - send_deadline_tick) >= 0) {
                    terminal_print_line("mq send timeout ok");
                    break;
                }
                continue;
            }

            vfs_close(read_fd);
            vfs_close(write_fd);
            task_mq_unlink(queue_id);
            if (rc == 0) {
                terminal_print_line("[error] mq demo: el timeout de send no se activo");
            } else {
                terminal_print_line("[error] mq demo: fallo inesperado en send");
            }
            return 1;
        }

        vfs_close(read_fd);
        vfs_close(write_fd);
        task_mq_unlink(queue_id);
        terminal_print_line("mq demo ok");
        return 1;
    }

    terminal_print_line("uso: mq [list|create <depth> <msg_size>|open <id> <r|w|rw> [nonblock]|send <id> <texto>|recv <id>|sendwait <id> <timeout_ms> <texto>|recvwait <id> <timeout_ms>|unlink <id>|demo]");
    return 1;
}

/* ---- cmd_task ---- */
static int cmd_task(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;

    terminal_print("Tarea en CPU: ");
    terminal_print_line(task_current_name() ? task_current_name() : "ninguna");
    terminal_print("En ejecucion: ");
    terminal_print_line(task_is_running() ? "si" : "no");
    terminal_print("Foreground: ");
    if (task_has_foreground_task()) {
        terminal_print_uint((unsigned int)task_foreground_task_id());
        terminal_put_char('\n');
    } else {
        terminal_print_line("ninguna");
    }
    terminal_print("Cancelacion pendiente: ");
    terminal_print_line(task_cancel_requested() ? "si" : "no");
    terminal_print("Prioridad: ");
    terminal_print_line(task_priority_name(task_current_priority()));
    return 1;
}

static int cmd_mem(int argc, const char* argv[], int background) {
    heap_stats_t stats;

    (void)argc;
    (void)argv;
    (void)background;

    heap_get_stats(&stats);
    terminal_print("Heap total: ");
    terminal_print_uint(stats.total_size);
    terminal_put_char('\n');
    terminal_print("Heap usado: ");
    terminal_print_uint(stats.used_size);
    terminal_put_char('\n');
    terminal_print("Heap libre: ");
    terminal_print_uint(stats.free_size);
    terminal_put_char('\n');
    terminal_print("Bloques: ");
    terminal_print_uint(stats.block_count);
    terminal_print(" (libres ");
    terminal_print_uint(stats.free_block_count);
    terminal_print_line(")");

    terminal_print("Frames fisicos: ");
    terminal_print_uint(physmem_frame_count());
    terminal_put_char('\n');
    terminal_print("Memoria fisica total: ");
    terminal_print_uint(physmem_total_bytes());
    terminal_put_char('\n');
    terminal_print("Memoria fisica usada: ");
    terminal_print_uint(physmem_used_bytes());
    terminal_put_char('\n');
    terminal_print("Memoria fisica libre: ");
    terminal_print_uint(physmem_free_bytes());
    terminal_put_char('\n');
    terminal_print("Paging: ");
    terminal_print_line(paging_is_enabled() ? "activo" : "inactivo");
    if (paging_is_enabled()) {
        terminal_print("Identity mapped: ");
        terminal_print_uint(paging_mapped_bytes());
        terminal_put_char('\n');
    }

    return 1;
}

static int cmd_wait(int argc, const char* argv[], int background) {
    wait_task_data_t data;
    int id;

    if (argc < 2) {
        terminal_print_line("Uso: wait <id> [&]");
        return 1;
    }

    data.event_id = parser_parse_integer(argv[1], -1);
    data.armed = 0;

    if (data.event_id < 0) {
        terminal_print_line("Evento invalido");
        return 1;
    }

    id = task_spawn("wait", wait_task_step, &data, sizeof(data), background ? 0 : 1);
    if (id < 0) {
        terminal_print_line("No se pudo crear la tarea wait");
        return 1;
    }

    if (background) {
        shell_print_job_started(id, "wait");
        return 1;
    }

    terminal_print("Wait en evento ");
    terminal_print_uint((unsigned int)data.event_id);
    terminal_put_char('\n');
    return 0;
}

static int cmd_signal(int argc, const char* argv[], int background) {
    int event_id;
    int woken;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: signal <id>");
        return 1;
    }

    event_id = parser_parse_integer(argv[1], -1);
    if (event_id < 0) {
        terminal_print_line("Evento invalido");
        return 1;
    }

    woken = task_signal_event(event_id);
    terminal_print("Tareas despertadas: ");
    terminal_print_uint((unsigned int)woken);
    terminal_put_char('\n');
    return 1;
}

static int cmd_mouse(int argc, const char* argv[], int background) {
    mouse_state_t state;

    (void)argc;
    (void)argv;
    (void)background;

    mouse_get_state(&state);

    terminal_print("Mouse PS/2: ");
    terminal_print_line(state.enabled ? "activo" : "inactivo");
    terminal_print("Posicion acumulada: ");
    shell_print_int(state.x);
    terminal_print(", ");
    shell_print_int(state.y);
    terminal_put_char('\n');
    terminal_print("Botones: ");
    terminal_print_uint((unsigned int)state.buttons);
    terminal_put_char('\n');
    terminal_print("Paquetes: ");
    terminal_print_uint(state.packets_received);
    terminal_put_char('\n');
    terminal_print("Perdidos cola: ");
    terminal_print_uint(state.packets_dropped);
    terminal_put_char('\n');
    terminal_print("Invalidos/resync: ");
    terminal_print_uint(state.invalid_packets);
    terminal_print(" / ");
    terminal_print_uint(state.resync_count);
    terminal_put_char('\n');
    terminal_print("Overflow: ");
    terminal_print_uint(state.overflow_packets);
    terminal_put_char('\n');
    terminal_print("Fallos init: ");
    terminal_print_uint(state.init_failures);
    terminal_put_char('\n');
    return 1;
}

static int cmd_dmesg(int argc, const char* argv[], int background) {
    (void)background;

    if (argc >= 2 && str_equals_ignore_case(argv[1], "clear")) {
        klog_clear();
        terminal_print_line("Logs del kernel borrados");
        return 1;
    }

    if (klog_count() == 0) {
        terminal_print_line("Sin logs del kernel");
        return 1;
    }

    klog_dump_to_terminal();
    return 1;
}

static int cmd_gfxdemo(int argc, const char* argv[], int background) {
    int panel_width = 180;
    int panel_height = 120;
    int x;
    int y;

    (void)argc;
    (void)argv;
    (void)background;

    if (!fb_active()) {
        terminal_print_line("Framebuffer no activo");
        return 1;
    }

    if ((int)fb_width() < panel_width + 20 || (int)fb_height() < panel_height + 20) {
        terminal_print_line("Resolucion insuficiente para gfxdemo");
        return 1;
    }

    x = (int)fb_width() - panel_width - 16;
    y = 16;

    fb_fill_rect(x, y, panel_width, panel_height, 0x101820);
    fb_draw_rect(x, y, panel_width, panel_height, 0x55CCFF);
    fb_fill_rect(x + 12, y + 12, 48, 28, 0xAA3344);
    fb_fill_rect(x + 72, y + 12, 96, 28, 0x3355AA);
    fb_draw_rect(x + 12, y + 52, 156, 56, 0x55FFAA);
    fb_draw_line(x + 12, y + 52, x + 167, y + 107, 0xFFFF55);
    fb_draw_line(x + 167, y + 52, x + 12, y + 107, 0xFF77AA);

    terminal_print_line("Demo grafica dibujada en framebuffer");
    return 1;
}

static int cmd_ls(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    char entry[VFS_NAME_MAX];
    int  fd, idx = 0;

    (void)background;

    if (argc >= 2)
        shell_resolve_path(argv[1], path, sizeof(path));
    else {
        unsigned int _k;
        for (_k = 0; shell_cwd[_k] && _k < VFS_PATH_MAX - 1U; _k++)
            path[_k] = shell_cwd[_k];
        path[_k] = '\0';
    }

    fd = vfs_open_flags(path, VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd < 0) {
        terminal_print("[error] no se pudo abrir: ");
        terminal_print_line(path);
        return 1;
    }

    while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0) {
        char full[VFS_PATH_MAX];
        vfs_node_t* n;
        unsigned int fl = 0;
        unsigned int pi;

        /* Build full path for the entry */
        for (pi = 0; path[pi] && pi < VFS_PATH_MAX - 2U; pi++) full[pi] = path[pi];
        if (pi > 0 && full[pi - 1] != '/') full[pi++] = '/';
        {
            unsigned int ei;
            for (ei = 0; entry[ei] && pi < VFS_PATH_MAX - 1U; ei++, pi++)
                full[pi] = entry[ei];
        }
        full[pi] = '\0';

        n = vfs_resolve(full);
        if (n) {
            fl = n->flags;
            /* Free the dynamically-allocated resolve result */
            if (n->flags & VFS_FLAG_DYNAMIC)
                kfree(n);
        }

        if (fl & VFS_FLAG_DIR) {
            shell_print_text_with_color(entry, 0x0E);
            terminal_put_char('/');
        } else {
            shell_print_text_with_color(entry, 0x0F);
        }
        terminal_put_char('\n');
        idx++;
    }

    vfs_close(fd);

    if (idx == 0)
        terminal_print_line("(directorio vacio)");

    return 1;
}

static int cmd_cat(int argc, const char* argv[], int background) {
    unsigned char buf[512];
    char path[VFS_PATH_MAX];
    int fd, n, total = 0;

    (void)background;

    if (argc < 2) {
        if (shell_has_pipe_input()) {
            terminal_print(shell_pipe_buffer);
            if (shell_pipe_buffer[shell_pipe_length - 1] != '\n')
                terminal_put_char('\n');
            return 1;
        }
        terminal_print_line("Uso: cat <ruta>");
        return 1;
    }

    shell_resolve_path(argv[1], path, sizeof(path));

    fd = vfs_open_flags(path, VFS_O_RDONLY);
    if (fd < 0) {
        terminal_print("[error] Archivo no encontrado: ");
        terminal_print_line(path);
        return 1;
    }

    {
        char last_char = '\n';
        while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            terminal_print((const char*)buf);
            last_char = buf[n - 1];
            total += n;
        }
        if (total > 0 && last_char != '\n')
            terminal_put_char('\n');
    }

    vfs_close(fd);
    return 1;
}

static int cmd_grep(int argc, const char* argv[], int background) {
    const char* pattern;
    char* text;
    unsigned int text_length = 0;
    unsigned int len;
    unsigned int i;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: grep <patron> [ruta]");
        return 1;
    }

    pattern = argv[1];
    text = 0;

    if (argc >= 3) {
        char path[VFS_PATH_MAX];
        if (!shell_load_text_argument_or_pipe(argv[2], &text, &text_length, path, sizeof(path))) {
            return 1;
        }
    } else if (shell_has_pipe_input()) {
        if (!shell_load_text_argument_or_pipe(0, &text, &text_length, 0, 0)) {
            return 1;
        }
    } else {
        terminal_print_line("Uso: grep <patron> [ruta]  o  comando | grep <patron>");
        return 1;
    }

    len = str_length(text);
    i = 0;
    while (i <= len) {
        unsigned int start = i;
        unsigned int end = i;
        char saved;

        while (text[end] != '\0' && text[end] != '\n') end++;
        saved = text[end];
        text[end] = '\0';

        if (shell_text_contains(&text[start], pattern)) {
            terminal_print(&text[start]);
            terminal_put_char('\n');
        }

        if (saved == '\0') break;
        text[end] = saved;
        i = end + 1;
    }

    kfree(text);

    return 1;
}

static int cmd_source(int argc, const char* argv[], int background) {
    unsigned int size;
    char text[SHELL_PIPE_MAX];
    char* buffer;
    int read_result;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: source <NOMBRE>");
        return 1;
    }

    read_result = shell_read_text_source(argv[1], text, sizeof(text));
    if (read_result < 0) {
        terminal_print_line("Script no encontrado");
        return 1;
    }

    size = (unsigned int)read_result;
    buffer = (char*)kmalloc(size + 1);
    if (buffer == 0) {
        terminal_print_line("Sin memoria para leer script");
        return 1;
    }

    for (unsigned int i = 0; i < size; i++) {
        buffer[i] = text[i];
    }
    buffer[size] = '\0';

    shell_execute_script_text(buffer);
    kfree(buffer);
    return 1;
}

static int cmd_elfinfo(int argc, const char* argv[], int background) {
    unsigned int size;
    unsigned char* image;
    int read;
    elf_image_info_t info;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: elfinfo <NOMBRE>");
        return 1;
    }

    size = fs_size(argv[1]);
    if (size == 0) {
        terminal_print_line("No existe ese archivo");
        return 1;
    }

    image = (unsigned char*)kmalloc(size);
    if (image == 0) {
        terminal_print_line("Sin memoria para leer ELF");
        return 1;
    }

    read = fs_read_bytes(argv[1], image, size);
    if (read < 0 || (unsigned int)read != size) {
        kfree(image);
        terminal_print_line("No se pudo leer el archivo");
        return 1;
    }

    if (!elf_parse_image(image, size, &info)) {
        kfree(image);
        terminal_print_line("No es un ELF32 i386 valido");
        return 1;
    }

    terminal_print("ELF valido: ");
    terminal_print_line(argv[1]);
    terminal_print("Entry: ");
    terminal_print_hex(info.entry);
    terminal_put_char('\n');
    terminal_print("Tipo: ");
    terminal_print_uint(info.type);
    terminal_put_char('\n');
    terminal_print("Machine: ");
    terminal_print_uint(info.machine);
    terminal_put_char('\n');
    terminal_print("Program headers: ");
    terminal_print_uint(info.program_header_count);
    terminal_put_char('\n');
    terminal_print("Loadable segments: ");
    terminal_print_uint(info.loadable_segments);
    terminal_put_char('\n');

    kfree(image);
    return 1;
}

static int cmd_exec(int argc, const char* argv[], int background) {
    int id;
    const char* envp[16];
    char env_storage[16][SHELL_ENV_ENTRY_MAX];
    int envc;

    if (argc < 2) {
        terminal_print_line("Uso: exec <NOMBRE> [args...] [&]");
        return 1;
    }

    /* argv[1] = path; forward argv[1..] as the ELF's argv */
    envc = shell_build_envp(envp, env_storage, 16);

    id = usermode_spawn_elf_vfs_argv(argv[1],
                                     argc - 1, (const char* const*)(argv + 1),
                                     envc, (const char* const*)envp,
                                     background ? 0 : 1);
    if (id < 0) {
        terminal_print_line("No se pudo cargar o ejecutar el ELF");
        return 1;
    }

    if (background) {
        shell_print_job_started(id, argv[1]);
        return 1;
    }

    terminal_print("Ejecutando ELF user mode: ");
    terminal_print_line(argv[1]);
    return 0;
}

static int cmd_repeat(int argc, const char* argv[], int background) {
    int count;
    int failures = 0;
    char command_line[SHELL_PIPE_MAX];

    (void)background;

    if (argc < 3) {
        terminal_print_line("Uso: repeat <N> <comando...>");
        return 1;
    }

    count = parser_parse_integer(argv[1], 0);
    if (count <= 0) {
        terminal_print_line("N debe ser > 0");
        return 1;
    }

    shell_join_args_to_buffer(argc, argv, 2, command_line, (int)sizeof(command_line));

    terminal_print("Repitiendo ");
    terminal_print_uint((unsigned int)count);
    terminal_print(" veces: ");
    terminal_print_line(command_line);

    for (int i = 0; i < count; i++) {
        int result = shell_execute_line(command_line);
        if (result == 0) {
            /* foreground command launched; let scheduler run */
            syscall_yield();
        }
        if (result < 0) {
            failures++;
        }
    }

    terminal_print("repeat: completado, fallos=");
    terminal_print_uint((unsigned int)failures);
    terminal_put_char('\n');
    return 1;
}

static int cmd_getpid(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    terminal_print("PID: ");
    terminal_print_uint((unsigned int)task_current_id());
    terminal_put_char('\n');
    return 1;
}

static int cmd_whoami(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    terminal_print_line(ugdb_username(task_current_euid()));
    return 1;
}

static int cmd_id(int argc, const char* argv[], int background) {
    unsigned int uid  = task_current_uid();
    unsigned int gid  = task_current_gid();
    unsigned int euid = task_current_euid();
    unsigned int egid = task_current_egid();
    unsigned int groups[TASK_MAX_SUPP_GROUPS];
    int gcount;
    (void)argc;
    (void)argv;
    (void)background;

    terminal_print("uid="); terminal_print_uint(uid);
    terminal_print("("); terminal_print(ugdb_username(uid)); terminal_print(") ");
    terminal_print("gid="); terminal_print_uint(gid);
    terminal_print("("); terminal_print(ugdb_groupname(gid)); terminal_print(") ");
    terminal_print("euid="); terminal_print_uint(euid);
    terminal_print("("); terminal_print(ugdb_username(euid)); terminal_print(") ");
    terminal_print("egid="); terminal_print_uint(egid);
    terminal_print("("); terminal_print(ugdb_groupname(egid)); terminal_print(")");

    gcount = task_get_groups(groups, TASK_MAX_SUPP_GROUPS);
    terminal_print(" groups=");
    if (gcount <= 0) {
        terminal_print("[]");
    } else {
        terminal_print("[");
        for (int i = 0; i < gcount && i < TASK_MAX_SUPP_GROUPS; i++) {
            if (i > 0) terminal_print(",");
            terminal_print_uint(groups[i]);
            terminal_print("(");
            terminal_print(ugdb_groupname(groups[i]));
            terminal_print(")");
        }
        terminal_print("]");
    }
    terminal_put_char('\n');
    return 1;
}

static int cmd_groups(int argc, const char* argv[], int background) {
    unsigned int groups[TASK_MAX_SUPP_GROUPS];
    int gcount;
    (void)argc;
    (void)argv;
    (void)background;

    terminal_print("egid=");
    terminal_print_uint(task_current_egid());
    terminal_print("(");
    terminal_print(ugdb_groupname(task_current_egid()));
    terminal_print(")");

    gcount = task_get_groups(groups, TASK_MAX_SUPP_GROUPS);
    if (gcount > 0) {
        terminal_print(" supp=");
        for (int i = 0; i < gcount && i < TASK_MAX_SUPP_GROUPS; i++) {
            if (i > 0) terminal_print(",");
            terminal_print_uint(groups[i]);
            terminal_print("(");
            terminal_print(ugdb_groupname(groups[i]));
            terminal_print(")");
        }
    }
    terminal_put_char('\n');
    return 1;
}

static int cmd_su(int argc, const char* argv[], int background) {
    const ugdb_user_t* user = 0;
    unsigned int parsed_uid = 0U;
    int has_numeric_uid = 1;

    (void)background;
    if (argc < 2) {
        terminal_print_line("Uso: su <usuario|uid>");
        return 1;
    }

    user = ugdb_find_by_name(argv[1]);
    if (!user) {
        /* Alias robustos para nombres built-in. */
        if (str_equals_ignore_case(argv[1], "root")) {
            user = ugdb_find_by_uid(0U);
        } else if (str_equals_ignore_case(argv[1], "user")) {
            user = ugdb_find_by_uid(1U);
        }
    }

    if (!user) {
        for (int i = 0; argv[1][i] != '\0'; i++) {
            char c = argv[1][i];
            if (c < '0' || c > '9') {
                has_numeric_uid = 0;
                break;
            }
            parsed_uid = parsed_uid * 10U + (unsigned int)(c - '0');
        }
        if (has_numeric_uid && argv[1][0] != '\0') {
            user = ugdb_find_by_uid(parsed_uid);
        }
    }

    if (!user) {
        terminal_print("su: usuario no encontrado: ");
        terminal_print_line(argv[1]);
        return 1;
    }

    /* Always require the target user's password when switching to a different account. */
    if (user->uid != task_current_euid()) {
        if (!ugdb_check_password(user->uid, "")) {
            /* A password is set on the target account -- prompt for it */
            char su_pw[16];
            terminal_print("Contraseña: ");
            shell_read_secret_line(su_pw, sizeof(su_pw));
            if (!ugdb_check_password(user->uid, su_pw)) {
                terminal_print_line("su: autenticación fallida");
                return 1;
            }
        }
    }

    /* Pre-create the home directory while we still have root privileges.
       Also chown it to the new user so they can write files (e.g. .lythrc).
       This MUST happen before task_force_identity; non-root cannot chown. */
    {
        char pre_home[VFS_PATH_MAX];
        shell_build_user_home_path(user->name, pre_home, sizeof(pre_home));
        shell_mkdir_p(pre_home); /* idempotent */
        vfs_chown(pre_home, user->uid, user->gid);
    }

    task_force_identity(user->uid, user->gid);
    shell_apply_user_session(0);
    terminal_print("sesion -> ");
    terminal_print(user->name);
    terminal_print(" (uid=");
    terminal_print_uint(user->uid);
    terminal_print_line(")");
    return 1;
}

static int cmd_yield(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    syscall_yield();
    terminal_print_line("yield enviado al scheduler");
    return 1;
}

void shell_init(void) {
    shell_cwd[0] = '/';
    shell_cwd[1] = '\0';
    shell_home_path[0] = '\0';
    shell_env_set("OS", "Lyth");
    shell_env_set("ARCH", "i386");
    shell_apply_theme("default", 0);
    shell_apply_user_session(1);

    /* Pre-create home directories for all registered users so they exist
       from boot even before anyone runs 'su'.  We iterate possible UIDs
       while we still hold root privileges (euid=0). */
    {
        unsigned int uid;
        for (uid = 0; uid < UGDB_MAX_USERS; uid++) {
            const ugdb_user_t* u = ugdb_find_by_uid(uid);
            if (u) {
                char pre_home[VFS_PATH_MAX];
                shell_build_user_home_path(u->name, pre_home, sizeof(pre_home));
                shell_mkdir_p(pre_home);
                /* Assign ownership while still root */
                vfs_chown(pre_home, u->uid, u->gid);
            }
        }
    }

    shell_print_banner();
}

static int shell_execute_line_raw(const char* line) {
    char tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char expanded_tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char filtered_tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char* input_text = 0;
    char* captured_output = 0;
    const char* argv[SHELL_MAX_ARGS];
    const char* filtered_argv[SHELL_MAX_ARGS];
    int argc;
    int filtered_argc;
    int background = 0;
    int input_redirect = 0;
    int output_redirect = 0;
    int append_redirect = 0;
    int capture_active = 0;
    char input_redirection[SHELL_TOKEN_MAX];
    char output_redirection[SHELL_TOKEN_MAX];
    int capture_length;

    if (line == 0) {
        return 1;
    }

    argc = parser_parse_line(line, tokens, argv, SHELL_MAX_ARGS, SHELL_TOKEN_MAX);
    if (argc == 0) {
        return 1;
    }

    for (int i = 0; i < argc; i++) {
        shell_expand_token(tokens[i], expanded_tokens[i], SHELL_TOKEN_MAX);
        argv[i] = expanded_tokens[i];
    }

    if (str_equals(argv[argc - 1], "&")) {
        background = 1;
        argc--;
        if (argc == 0) {
            return 1;
        }
    }

    input_redirection[0] = '\0';
    output_redirection[0] = '\0';

    filtered_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (str_equals(argv[i], "<") || str_equals(argv[i], ">") || str_equals(argv[i], ">>")) {
            if (i + 1 >= argc) {
                terminal_print_line("Uso: comando [< entrada] [> salida]");
                return 1;
            }

            if (str_equals(argv[i], "<")) {
                input_redirect = 1;
                copy_bounded(input_redirection, argv[i + 1], SHELL_TOKEN_MAX);
            } else {
                output_redirect = 1;
                append_redirect = str_equals(argv[i], ">>") ? 1 : 0;
                copy_bounded(output_redirection, argv[i + 1], SHELL_TOKEN_MAX);
            }

            i++;
            continue;
        }

        copy_bounded(filtered_tokens[filtered_argc], argv[i], SHELL_TOKEN_MAX);
        filtered_argv[filtered_argc] = filtered_tokens[filtered_argc];
        filtered_argc++;

        if (filtered_argc >= SHELL_MAX_ARGS) {
            break;
        }
    }

    if (filtered_argc == 0) {
        return 1;
    }

    if (input_redirect) {
        char in_path[VFS_PATH_MAX];
        unsigned int input_length = 0;

        if (!shell_load_text_argument_or_pipe(input_redirection, &input_text, &input_length, in_path, sizeof(in_path))) {
            return 1;
        }

        shell_set_pipe_input_owned(input_text, input_length);
        input_text = 0;
    }

    if (output_redirect) {
        if (!terminal_capture_begin_dynamic(256U)) {
            if (input_redirect) {
                shell_clear_pipe_input();
            }
            return 1;
        }
        capture_active = 1;
    }

    for (int i = 0; i < command_count; i++) {
        if (command_name_matches(filtered_argv[0], commands[i].name)) {
            int result = commands[i].fn(filtered_argc, filtered_argv, background);

            if (capture_active) {
                captured_output = terminal_capture_end_dynamic((unsigned int*)&capture_length);
                shell_redir_store(output_redirection, captured_output, (unsigned int)capture_length, append_redirect);
                kfree(captured_output);
            }

            if (input_redirect) {
                shell_clear_pipe_input();
            }

            return result;
        }
    }

    if (input_redirect) {
        shell_clear_pipe_input();
    }

    terminal_print("Comando no reconocido: ");
    terminal_print_line(filtered_argv[0]);

    if (capture_active) {
        captured_output = terminal_capture_end_dynamic((unsigned int*)&capture_length);
        shell_redir_store(output_redirection, captured_output, (unsigned int)capture_length, append_redirect);
        kfree(captured_output);
    }

    return 1;
}

static int shell_capture_command_output(const char* line, char** out_buffer, unsigned int* out_length) {
    if (out_buffer == 0 || out_length == 0) {
        return 0;
    }

    *out_buffer = 0;
    *out_length = 0;

    if (!terminal_capture_begin_dynamic(256U)) {
        return 0;
    }

    shell_execute_line_raw(line);
    *out_buffer = terminal_capture_end_dynamic(out_length);
    return *out_buffer != 0;
}

static int shell_execute_pipeline(const char* line) {
    unsigned int length;
    char* line_copy;
    unsigned int start = 0;
    unsigned int index;
    int last_result = 1;

    if (line == 0) {
        return 1;
    }

    length = str_length(line);
    line_copy = (char*)kmalloc(length + 1U);
    if (line_copy == 0) {
        terminal_print_line("[error] Memoria insuficiente para ejecutar pipeline");
        return 1;
    }

    for (index = 0; index <= length; index++) {
        line_copy[index] = line[index];
    }

    index = 0;
    while (index <= length) {
        char current = line_copy[index];
        int is_separator = 0;

        if (current == '\0') {
            is_separator = 1;
        } else if ((unsigned int)shell_find_unquoted_char(line_copy + start, '|') == index - start && current == '|') {
            is_separator = 1;
        }

        if (is_separator) {
            char* segment = line_copy + start;
            char* captured_output;
            unsigned int captured_length;
            int has_more = current == '|';

            line_copy[index] = '\0';
            while (*segment == ' ' || *segment == '\t') {
                segment++;
            }
            shell_trim_trailing_spaces(segment);

            if (segment[0] == '\0') {
                terminal_print_line("Uso: comando1 | comando2");
                shell_clear_pipe_input();
                kfree(line_copy);
                return 1;
            }

            if (has_more) {
                if (shell_find_unquoted_char(segment, '>') >= 0) {
                    terminal_print_line("No se admite redireccion de salida antes del final de un pipe");
                    shell_clear_pipe_input();
                    kfree(line_copy);
                    return 1;
                }

                if (!shell_capture_command_output(segment, &captured_output, &captured_length)) {
                    shell_clear_pipe_input();
                    kfree(line_copy);
                    return 1;
                }

                shell_set_pipe_input_owned(captured_output, captured_length);
            } else {
                last_result = shell_execute_line_raw(segment);
            }

            start = index + 1U;
        }

        if (current == '\0') {
            break;
        }

        if (current == '\\' && line_copy[index + 1U] != '\0') {
            index += 2U;
            continue;
        }

        if (current == '\'' || current == '"') {
            char quote = current;
            index++;
            while (index <= length && line_copy[index] != '\0') {
                if (line_copy[index] == '\\' && line_copy[index + 1U] != '\0') {
                    index += 2U;
                    continue;
                }
                if (line_copy[index] == quote) {
                    break;
                }
                index++;
            }
        }

        index++;
    }

    shell_clear_pipe_input();
    kfree(line_copy);
    return last_result;
}

int shell_execute_line(const char* line) {
    char left_line[SHELL_PIPE_MAX];
    int and_index;
    int result;

    if (line == 0) {
        return 1;
    }

    and_index = shell_find_unquoted_andand(line);
    if (and_index >= 0) {
        int i;
        const char* right_start;

        if (and_index <= 0 || and_index >= (int)sizeof(left_line) - 1) {
            terminal_print_line("Uso: comando1 && comando2");
            return 1;
        }

        for (i = 0; i < and_index && i < (int)sizeof(left_line) - 1; i++) {
            left_line[i] = line[i];
        }
        left_line[i] = '\0';
        shell_trim_trailing_spaces(left_line);

        right_start = line + and_index + 2;
        while (*right_start == ' ' || *right_start == '\t') {
            right_start++;
        }

        if (left_line[0] == '\0' || *right_start == '\0') {
            terminal_print_line("Uso: comando1 && comando2");
            return 1;
        }

        result = shell_execute_line(left_line);
        if (result == 0) {
            syscall_yield();
        }
        return shell_execute_line(right_start);
    }

    if (shell_find_unquoted_char(line, '|') < 0) {
        return shell_execute_line_raw(line);
    }

    return shell_execute_pipeline(line);
}

int shell_complete_command(const char* prefix, const char* matches[], int max_matches) {
    int found = 0;

    if (prefix == 0) {
        return 0;
    }

    for (int i = 0; i < command_count; i++) {
        if (str_starts_with_ignore_case(commands[i].name, prefix)) {
            if (found < max_matches) {
                matches[found] = commands[i].name;
            }
            found++;
        }
    }

    return found;
}

/* ============================================================
 *  Path resolution helper
 *
 *  Resolves 'input' against shell_cwd:
 *    absolute path  → used as-is
 *    ".."           → go up one component
 *    "."            → same as cwd
 *    anything else  → appended to cwd
 *
 *  Result is written to out[out_size] as an absolute VFS path.
 * ============================================================ */
static void shell_resolve_path(const char* input, char* out, unsigned int out_size) {
    char tmp[VFS_PATH_MAX];
    unsigned int ti = 0;
    unsigned int i;

    if (!input || !out || out_size == 0) return;

    if (input[0] == '/') {
        /* absolute — copy verbatim */
        for (i = 0; input[i] && i < out_size - 1U; i++) out[i] = input[i];
        out[i] = '\0';
        return;
    }

    /* Start from cwd */
    for (i = 0; shell_cwd[i] && ti < VFS_PATH_MAX - 1U; i++)
        tmp[ti++] = shell_cwd[i];
    /* Ensure trailing slash for component appending */
    if (ti > 0 && tmp[ti - 1] != '/') {
        if (ti < VFS_PATH_MAX - 1U) tmp[ti++] = '/';
    }
    tmp[ti] = '\0';

    /* Walk input component by component */
    i = 0;
    while (input[i] != '\0') {
        char comp[VFS_NAME_MAX];
        unsigned int ci = 0;

        /* Extract one component */
        while (input[i] != '\0' && input[i] != '/' && ci < VFS_NAME_MAX - 1U)
            comp[ci++] = input[i++];
        comp[ci] = '\0';
        if (input[i] == '/') i++;  /* skip separator */
        if (ci == 0) continue;     /* skip double slashes */

        if (comp[0] == '.' && comp[1] == '\0') {
            /* "." – stay */
        } else if (comp[0] == '.' && comp[1] == '.' && comp[2] == '\0') {
            /* ".." – strip last component */
            if (ti > 1) {
                ti--;                            /* remove trailing '/' */
                while (ti > 1 && tmp[ti - 1] != '/') ti--;
                /* leave the '/' that follows the parent */
            }
            tmp[ti] = '\0';
        } else {
            /* Append component */
            unsigned int k;
            for (k = 0; comp[k] && ti < VFS_PATH_MAX - 2U; k++)
                tmp[ti++] = comp[k];
            if (input[i] != '\0') {   /* more components follow */
                if (ti < VFS_PATH_MAX - 1U) tmp[ti++] = '/';
            }
            tmp[ti] = '\0';
        }
    }

    /* Normalise: strip trailing slash unless root */
    while (ti > 1 && tmp[ti - 1] == '/') ti--;
    tmp[ti] = '\0';

    if (ti == 0) { out[0] = '/'; out[1] = '\0'; return; }

    for (i = 0; i < ti && i < out_size - 1U; i++) out[i] = tmp[i];
    out[i] = '\0';
}

/* ---- cmd_pwd ---- */
static int cmd_pwd(int argc, const char* argv[], int background) {
    (void)argc; (void)argv; (void)background;
    terminal_print_line(shell_cwd);
    return 1;
}

/* ---- cmd_cd ---- */
static int cmd_cd(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    vfs_node_t* node;
    const char* home;

    (void)background;

    if (argc < 2) {
        /* cd with no args → go to HOME, fallback to root */
        home = shell_env_get("HOME");
        if (home && home[0] != '\0') {
            shell_resolve_path(home, path, sizeof(path));
            node = vfs_resolve(path);
            if (node && (node->flags & VFS_FLAG_DIR)) {
                unsigned int k;
                for (k = 0; path[k] && k < VFS_PATH_MAX - 1U; k++)
                    shell_cwd[k] = path[k];
                shell_cwd[k] = '\0';
                return 1;
            }
        }
        shell_cwd[0] = '/';
        shell_cwd[1] = '\0';
        return 1;
    }

    shell_resolve_path(argv[1], path, sizeof(path));

    node = vfs_resolve(path);
    if (!node) {
        terminal_print("[error] no existe: ");
        terminal_print_line(path);
        return 1;
    }
    if (!(node->flags & VFS_FLAG_DIR)) {
        terminal_print("[error] no es un directorio: ");
        terminal_print_line(path);
        return 1;
    }

    /* Update cwd */
    {
        unsigned int k;
        for (k = 0; path[k] && k < VFS_PATH_MAX - 1U; k++)
            shell_cwd[k] = path[k];
        shell_cwd[k] = '\0';
    }

    return 1;
}

/* ---- cmd_vfs ---- */
static int cmd_vfs(int argc, const char* argv[], int background) {
    (void)background;

    /* vfs              → show mounts + fd table summary */
    /* vfs ls [path]    → readdir via VFS fd */
    /* vfs cat <path>   → read file via VFS fd */

    if (argc == 1) {
        /* Show mounted filesystems */
        shell_print_text_with_color("VFS mounts\n", 0x0B);
        shell_print_text_with_color("  /  \xe2\x86\x92  ramfs (in-memory)\n", 0x0F);
        terminal_put_char('\n');

        /* Show a summary: how many files accessible through VFS */
        int fd = vfs_open_flags("/", VFS_O_RDONLY | VFS_O_DIRECTORY);
        if (fd >= 0) {
            char entry[VFS_NAME_MAX];
            int count = 0;

            shell_print_text_with_color("Archivos accesibles via VFS (fd=", 0x07);
            terminal_print_uint((unsigned int)fd);
            shell_print_text_with_color("):\n", 0x07);

            while (vfs_readdir(fd, (unsigned int)count, entry, sizeof(entry)) == 0) {
                terminal_print("  ");
                terminal_print_line(entry);
                count++;
            }

            if (count == 0) {
                terminal_print_line("  (sin entradas)");
            }

            vfs_close(fd);
            terminal_put_char('\n');
            terminal_print("Total: ");
            terminal_print_uint((unsigned int)count);
            terminal_print_line(" archivos");
        } else {
            terminal_print_line("[error] No se pudo abrir /");
        }
        return 1;
    }

    /* vfs ls [path] */
    if (str_equals_ignore_case(argv[1], "ls")) {
        char path[VFS_PATH_MAX];
        int fd;
        int idx = 0;
        char entry[VFS_NAME_MAX];

        if (argc >= 3)
            shell_resolve_path(argv[2], path, sizeof(path));
        else {
            unsigned int _k;
            for (_k = 0; shell_cwd[_k] && _k < VFS_PATH_MAX - 1U; _k++)
                path[_k] = shell_cwd[_k];
            path[_k] = '\0';
        }

        fd = vfs_open_flags(path, VFS_O_RDONLY | VFS_O_DIRECTORY);
        if (fd < 0) {
            terminal_print("[error] No se pudo abrir: ");
            terminal_print_line(path);
            return 1;
        }

        shell_print_text_with_color("VFS ls ", 0x0B);
        shell_print_text_with_color(path, 0x0F);
        terminal_put_char('\n');

        while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0) {
            terminal_print("  ");
            shell_print_text_with_color(entry, 0x0F);
            terminal_put_char('\n');
            idx++;
        }

        if (idx == 0) terminal_print_line("  (directorio vacio)");
        vfs_close(fd);
        return 1;
    }

    /* vfs cat <path> */
    if (str_equals_ignore_case(argv[1], "cat")) {
        unsigned char buf[512];
        int n;
        int total = 0;

        if (argc < 3) {
            terminal_print_line("Uso: vfs cat <ruta>");
            return 1;
        }

        /* Resolve path relative to cwd */
        char path[VFS_PATH_MAX];
        shell_resolve_path(argv[2], path, sizeof(path));

        int fd = vfs_open_flags(path, VFS_O_RDONLY);
        if (fd < 0) {
            terminal_print("[error] Archivo no encontrado: ");
            terminal_print_line(path);
            return 1;
        }

        shell_print_text_with_color("--- VFS cat ", 0x08);
        shell_print_text_with_color(path, 0x0F);
        shell_print_text_with_color(" (fd=", 0x08);
        terminal_print_uint((unsigned int)fd);
        shell_print_text_with_color(") ---\n", 0x08);

        while ((n = vfs_read(fd, buf, sizeof(buf) - 1)) > 0) {
            buf[n] = '\0';
            terminal_print((const char*)buf);
            total += n;
        }

        if (total > 0 && buf[total > (int)(sizeof(buf)-1) ? sizeof(buf)-1 : total-1] != '\n')
            terminal_put_char('\n');

        shell_print_text_with_color("--- ", 0x08);
        terminal_print_uint((unsigned int)total);
        shell_print_text_with_color(" bytes leidos via fd ---\n", 0x08);

        vfs_close(fd);
        return 1;
    }

    /* vfs touch <path> */
    if (str_equals_ignore_case(argv[1], "touch")) {
        char path[VFS_PATH_MAX];

        if (argc < 3) {
            terminal_print_line("Uso: vfs touch <ruta>");
            return 1;
        }

        shell_resolve_path(argv[2], path, sizeof(path));

        if (vfs_create(path, VFS_FLAG_FILE) == 0) {
            shell_print_text_with_color("Creado: ", 0x0A);
            terminal_print_line(path);
        } else {
            terminal_print("[error] No se pudo crear: ");
            terminal_print_line(path);
        }
        return 1;
    }

    /* vfs rm <path> */
    if (str_equals_ignore_case(argv[1], "rm")) {
        char path[VFS_PATH_MAX];

        if (argc < 3) {
            terminal_print_line("Uso: vfs rm <ruta>");
            return 1;
        }

        shell_resolve_path(argv[2], path, sizeof(path));

        if (vfs_delete(path) == 0) {
            shell_print_text_with_color("Eliminado: ", 0x0C);
            terminal_print_line(path);
        } else {
            terminal_print("[error] No se pudo eliminar: ");
            terminal_print_line(path);
        }
        return 1;
    }

    /* vfs unlink <path> */
    if (str_equals_ignore_case(argv[1], "unlink")) {
        char path[VFS_PATH_MAX];

        if (argc < 3) {
            terminal_print_line("Uso: vfs unlink <ruta>");
            return 1;
        }

        shell_resolve_path(argv[2], path, sizeof(path));

        if (vfs_unlink(path) == 0) {
            shell_print_text_with_color("Eliminado: ", 0x0C);
            terminal_print_line(path);
        } else {
            terminal_print("[error] No se pudo eliminar: ");
            terminal_print_line(path);
        }
        return 1;
    }

    /* vfs stat <path> */
    if (str_equals_ignore_case(argv[1], "stat")) {
        char       path[VFS_PATH_MAX];
        vfs_stat_t st;

        if (argc < 3) {
            terminal_print_line("Uso: vfs stat <ruta>");
            return 1;
        }

        shell_resolve_path(argv[2], path, sizeof(path));
        if (vfs_stat(path, &st) != 0) {
            terminal_print("[error] No se pudo obtener stat de: ");
            terminal_print_line(path);
            return 1;
        }

        shell_print_text_with_color("Ruta: ", 0x0B);
        terminal_print_line(path);
        shell_print_text_with_color("Tipo: ", 0x0B);
        if (st.flags & VFS_FLAG_DIR) terminal_print_line("directorio");
        else if (st.flags & VFS_FLAG_FILE) terminal_print_line("archivo");
        else terminal_print_line("desconocido");
        shell_print_text_with_color("Tamano: ", 0x0B);
        terminal_print_uint(st.size);
        terminal_print_line(" bytes");
        shell_print_text_with_color("Flags: 0x", 0x0B);
        terminal_print_hex(st.flags);
        terminal_put_char('\n');
        shell_print_text_with_color("Modo: ", 0x0B);
        shell_print_mode_octal(st.mode);
        terminal_print_line(" (octal)");
        return 1;
    }

    /* vfs chmod <mode> <path> */
    if (str_equals_ignore_case(argv[1], "chmod")) {
        char path[VFS_PATH_MAX];
        unsigned int mode;
        int ok;

        if (argc < 4) {
            terminal_print_line("Uso: vfs chmod <modo_octal> <ruta>");
            return 1;
        }

        mode = shell_parse_mode_octal(argv[2], &ok);
        if (!ok) {
            terminal_print("[error] modo invalido: ");
            terminal_print_line(argv[2]);
            return 1;
        }

        shell_resolve_path(argv[3], path, sizeof(path));
        if (vfs_chmod(path, mode) != 0) {
            terminal_print("[error] No se pudo cambiar modo: ");
            terminal_print_line(path);
            return 1;
        }

        shell_print_text_with_color("Modo actualizado: ", 0x0A);
        terminal_print(path);
        terminal_print(" -> ");
        shell_print_mode_octal(mode);
        terminal_put_char('\n');
        return 1;
    }

    /* vfs cp <src> <dst> */
    if (str_equals_ignore_case(argv[1], "cp")) {
        char src[VFS_PATH_MAX];
        char dst[VFS_PATH_MAX];
        unsigned char buf[512];
        int sfd;
        int dfd;

        if (argc < 4) {
            terminal_print_line("Uso: vfs cp <origen> <destino>");
            return 1;
        }

        shell_resolve_path(argv[2], src, sizeof(src));
        shell_resolve_path(argv[3], dst, sizeof(dst));

        sfd = vfs_open(src);
        if (sfd < 0) {
            terminal_print("[error] No se pudo abrir origen: ");
            terminal_print_line(src);
            return 1;
        }

        if (vfs_fd_node(sfd) && (vfs_fd_node(sfd)->flags & VFS_FLAG_DIR)) {
            vfs_close(sfd);
            terminal_print_line("[error] cp de directorios no soportado");
            return 1;
        }

        if (vfs_create(dst, VFS_FLAG_FILE) != 0) {
            if (vfs_delete(dst) != 0 || vfs_create(dst, VFS_FLAG_FILE) != 0) {
                vfs_close(sfd);
                terminal_print("[error] No se pudo crear destino: ");
                terminal_print_line(dst);
                return 1;
            }
        }

        dfd = vfs_open(dst);
        if (dfd < 0) {
            vfs_close(sfd);
            terminal_print("[error] No se pudo abrir destino: ");
            terminal_print_line(dst);
            return 1;
        }

        while (1) {
            int n = vfs_read(sfd, buf, sizeof(buf));
            if (n < 0) {
                terminal_print_line("[error] fallo leyendo origen");
                vfs_close(dfd);
                vfs_close(sfd);
                return 1;
            }
            if (n == 0) break;
            if (vfs_write(dfd, buf, (unsigned int)n) != n) {
                terminal_print_line("[error] fallo escribiendo destino");
                vfs_close(dfd);
                vfs_close(sfd);
                return 1;
            }
        }

        vfs_close(dfd);
        vfs_close(sfd);
        shell_print_text_with_color("Copiado: ", 0x0A);
        terminal_print(src);
        terminal_print(" -> ");
        terminal_print_line(dst);
        return 1;
    }

    /* vfs mv <src> <dst> */
    if (str_equals_ignore_case(argv[1], "mv")) {
        char src[VFS_PATH_MAX];
        char dst[VFS_PATH_MAX];

        if (argc < 4) {
            terminal_print_line("Uso: vfs mv <origen> <destino>");
            return 1;
        }

        shell_resolve_path(argv[2], src, sizeof(src));
        shell_resolve_path(argv[3], dst, sizeof(dst));

        if (str_equals(src, dst)) return 1;

        if (vfs_rename(src, dst) != 0) {
            terminal_print("[error] No se pudo mover/renombrar: ");
            terminal_print(src);
            terminal_print(" -> ");
            terminal_print_line(dst);
            return 1;
        }

        shell_print_text_with_color("Movido: ", 0x0A);
        terminal_print(src);
        terminal_print(" -> ");
        terminal_print_line(dst);
        return 1;
    }

    /* vfs rename <src> <dst> */
    if (str_equals_ignore_case(argv[1], "rename")) {
        char src[VFS_PATH_MAX];
        char dst[VFS_PATH_MAX];

        if (argc < 4) {
            terminal_print_line("Uso: vfs rename <origen> <destino>");
            return 1;
        }

        shell_resolve_path(argv[2], src, sizeof(src));
        shell_resolve_path(argv[3], dst, sizeof(dst));

        if (vfs_rename(src, dst) != 0) {
            terminal_print("[error] No se pudo renombrar: ");
            terminal_print(src);
            terminal_print(" -> ");
            terminal_print_line(dst);
            return 1;
        }

        shell_print_text_with_color("Renombrado: ", 0x0A);
        terminal_print(src);
        terminal_print(" -> ");
        terminal_print_line(dst);
        return 1;
    }

    terminal_print_line("Uso: vfs [ls [ruta] | cat <ruta> | touch <ruta> | rm|unlink <ruta> | stat <ruta> | chmod <modo> <ruta> | cp <src> <dst> | mv|rename <src> <dst>]");
    return 1;
}

/* ---- cmd_touch ---- */
static int cmd_touch(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    (void)background;
    if (argc < 2) {
        terminal_print_line("Uso: touch <ruta>");
        return 1;
    }
    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_create(path, VFS_FLAG_FILE) != 0) {
        terminal_print("[error] No se pudo crear: ");
        terminal_print_line(path);
    }
    return 1;
}

/* ---- cmd_rm ---- */
static int cmd_rm(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    (void)background;
    if (argc < 2) {
        terminal_print_line("Uso: rm <ruta>");
        return 1;
    }
    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_unlink(path) == 0) {
        shell_print_text_with_color("Eliminado: ", 0x0C);
        terminal_print_line(path);
    } else {
        terminal_print("[error] No se pudo eliminar: ");
        terminal_print_line(path);
    }
    return 1;
}

/* ---- cmd_unlink ---- */
static int cmd_unlink(int argc, const char* argv[], int background) {
    return cmd_rm(argc, argv, background);
}

static unsigned int shell_parse_mode_octal(const char* s, int* ok) {
    unsigned int mode = 0;
    *ok = 0;
    if (!s || !s[0]) return 0;

    while (*s) {
        unsigned char c = (unsigned char)*s++;
        if (c < '0' || c > '7') return 0;
        mode = (mode << 3) | (unsigned int)(c - '0');
    }

    mode &= 0x01FFU;
    *ok = 1;
    return mode;
}

static void shell_print_mode_octal(unsigned int mode) {
    char out[4];
    out[0] = (char)('0' + ((mode >> 6) & 0x7U));
    out[1] = (char)('0' + ((mode >> 3) & 0x7U));
    out[2] = (char)('0' + ((mode >> 0) & 0x7U));
    out[3] = '\0';
    terminal_print(out);
}

/* ---- cmd_stat ---- */
static int cmd_stat(int argc, const char* argv[], int background) {
    char       path[VFS_PATH_MAX];
    vfs_stat_t st;

    (void)background;
    if (argc < 2) {
        terminal_print_line("Uso: stat <ruta>");
        return 1;
    }

    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_stat(path, &st) != 0) {
        terminal_print("[error] No se pudo obtener stat de: ");
        terminal_print_line(path);
        return 1;
    }

    shell_print_text_with_color("Ruta: ", 0x0B);
    terminal_print_line(path);
    shell_print_text_with_color("Tipo: ", 0x0B);
    if (st.flags & VFS_FLAG_DIR) terminal_print_line("directorio");
    else if (st.flags & VFS_FLAG_FILE) terminal_print_line("archivo");
    else terminal_print_line("desconocido");
    shell_print_text_with_color("Tamano: ", 0x0B);
    terminal_print_uint(st.size);
    terminal_print_line(" bytes");
    shell_print_text_with_color("Flags: 0x", 0x0B);
    terminal_print_hex(st.flags);
    terminal_put_char('\n');
    shell_print_text_with_color("Modo: ", 0x0B);
    shell_print_mode_octal(st.mode);
    terminal_print_line(" (octal)");
    shell_print_text_with_color("Owner: ", 0x0B);
    terminal_print_uint(st.uid);
    terminal_print(" (");
    terminal_print(ugdb_username(st.uid));
    terminal_print_line(")");
    shell_print_text_with_color("Group: ", 0x0B);
    terminal_print_uint(st.gid);
    terminal_print(" (");
    terminal_print(ugdb_groupname(st.gid));
    terminal_print_line(")");
    return 1;
}

/* ---- cmd_mkdir ---- */
static int cmd_mkdir(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    (void)background;
    if (argc < 2) {
        terminal_print_line("Uso: mkdir <ruta>");
        return 1;
    }
    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_create(path, VFS_FLAG_DIR) == 0) {
        shell_print_text_with_color("Directorio creado: ", 0x0A);
        terminal_print_line(path);
    } else {
        terminal_print("[error] No se pudo crear directorio: ");
        terminal_print_line(path);
    }
    return 1;
}

/* ---- cmd_chmod ---- */
static int cmd_chmod(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    unsigned int mode;
    int ok;

    (void)background;
    if (argc < 3) {
        terminal_print_line("Uso: chmod <modo_octal> <ruta>");
        terminal_print_line("  Ej: chmod 644 /hd0p1/archivo.txt");
        return 1;
    }

    mode = shell_parse_mode_octal(argv[1], &ok);
    if (!ok) {
        terminal_print("[error] modo invalido: ");
        terminal_print_line(argv[1]);
        return 1;
    }

    shell_resolve_path(argv[2], path, sizeof(path));
    if (vfs_chmod(path, mode) != 0) {
        terminal_print("[error] No se pudo cambiar modo: ");
        terminal_print_line(path);
        return 1;
    }

    shell_print_text_with_color("Modo actualizado: ", 0x0A);
    terminal_print(path);
    terminal_print(" -> ");
    shell_print_mode_octal(mode);
    terminal_put_char('\n');
    return 1;
}

/* ---- cmd_chown ---- */
static int cmd_chown(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    const ugdb_user_t* u = 0;
    const ugdb_group_t* g = 0;
    int uid_i;
    int gid_i;

    (void)background;
    if (argc < 4) {
        terminal_print_line("Uso: chown <uid|user> <gid|group> <ruta>");
        return 1;
    }

    u = ugdb_find_by_name(argv[1]);
    if (!u) {
        uid_i = parser_parse_integer(argv[1], -1);
        if (uid_i >= 0) u = ugdb_find_by_uid((unsigned int)uid_i);
    }
    if (!u) {
        terminal_print("chown: usuario invalido: ");
        terminal_print_line(argv[1]);
        return 1;
    }

    g = ugdb_find_group_by_name(argv[2]);
    if (!g) {
        gid_i = parser_parse_integer(argv[2], -1);
        if (gid_i >= 0) g = ugdb_find_group_by_gid((unsigned int)gid_i);
    }
    if (!g) {
        terminal_print("chown: grupo invalido: ");
        terminal_print_line(argv[2]);
        return 1;
    }

    shell_resolve_path(argv[3], path, sizeof(path));
    if (vfs_chown(path, u->uid, g->gid) != 0) {
        terminal_print("[error] chown fallo (permiso/ruta): ");
        terminal_print_line(path);
        return 1;
    }

    terminal_print("owner actualizado: ");
    terminal_print(path);
    terminal_print(" -> ");
    terminal_print(u->name);
    terminal_print(":");
    terminal_print(g->name);
    terminal_put_char('\n');
    return 1;
}

/* ---- cmd_cp ---- */
static int cmd_cp(int argc, const char* argv[], int background) {
    char src[VFS_PATH_MAX];
    char dst[VFS_PATH_MAX];
    unsigned char buf[512];
    int sfd;
    int dfd;

    (void)background;
    if (argc < 3) {
        terminal_print_line("Uso: cp <origen> <destino>");
        return 1;
    }

    shell_resolve_path(argv[1], src, sizeof(src));
    shell_resolve_path(argv[2], dst, sizeof(dst));

    sfd = vfs_open(src);
    if (sfd < 0) {
        terminal_print("[error] No se pudo abrir origen: ");
        terminal_print_line(src);
        return 1;
    }

    if (vfs_fd_node(sfd) && (vfs_fd_node(sfd)->flags & VFS_FLAG_DIR)) {
        vfs_close(sfd);
        terminal_print_line("[error] cp de directorios no soportado");
        return 1;
    }

    if (vfs_create(dst, VFS_FLAG_FILE) != 0) {
        if (vfs_delete(dst) != 0 || vfs_create(dst, VFS_FLAG_FILE) != 0) {
            vfs_close(sfd);
            terminal_print("[error] No se pudo crear destino: ");
            terminal_print_line(dst);
            return 1;
        }
    }

    dfd = vfs_open(dst);
    if (dfd < 0) {
        vfs_close(sfd);
        terminal_print("[error] No se pudo abrir destino: ");
        terminal_print_line(dst);
        return 1;
    }

    while (1) {
        int n = vfs_read(sfd, buf, sizeof(buf));
        if (n < 0) {
            terminal_print_line("[error] fallo leyendo origen");
            vfs_close(dfd);
            vfs_close(sfd);
            return 1;
        }
        if (n == 0) break;
        if (vfs_write(dfd, buf, (unsigned int)n) != n) {
            terminal_print_line("[error] fallo escribiendo destino");
            vfs_close(dfd);
            vfs_close(sfd);
            return 1;
        }
    }

    vfs_close(dfd);
    vfs_close(sfd);

    shell_print_text_with_color("Copiado: ", 0x0A);
    terminal_print(src);
    terminal_print(" -> ");
    terminal_print_line(dst);
    return 1;
}

/* ---- cmd_mv ---- */
static int cmd_mv(int argc, const char* argv[], int background) {
    char src[VFS_PATH_MAX];
    char dst[VFS_PATH_MAX];

    (void)background;
    if (argc < 3) {
        terminal_print_line("Uso: mv <origen> <destino>");
        return 1;
    }

    shell_resolve_path(argv[1], src, sizeof(src));
    shell_resolve_path(argv[2], dst, sizeof(dst));

    if (str_equals(src, dst)) return 1;

    if (vfs_rename(src, dst) != 0) {
        terminal_print("[error] No se pudo mover/renombrar: ");
        terminal_print(src);
        terminal_print(" -> ");
        terminal_print_line(dst);
        return 1;
    }

    shell_print_text_with_color("Movido: ", 0x0A);
    terminal_print(src);
    terminal_print(" -> ");
    terminal_print_line(dst);
    return 1;
}

/* ---- cmd_rename ---- */
static int cmd_rename(int argc, const char* argv[], int background) {
    return cmd_mv(argc, argv, background);
}

/* ================================================================
 *  User/group administration helpers and commands
 * ================================================================ */

/* Read a line from keyboard, echoing '*' for each character typed.
   Returns the number of characters stored (0 on Ctrl+C). */
static int shell_read_secret_line(char* buf, unsigned int max) {
    unsigned int pos = 0;
    input_event_t ev;
    if (!buf || max == 0) return 0;
    buf[0] = '\0';
    while (1) {
        if (!input_poll_event(&ev)) { task_yield(); continue; }
        if (ev.device_type != INPUT_DEVICE_KEYBOARD) continue;
        if (ev.type == INPUT_EVENT_ENTER || ev.type == INPUT_EVENT_CTRL_C) {
            if (ev.type == INPUT_EVENT_CTRL_C) buf[0] = '\0';
            break;
        }
        if (ev.type == INPUT_EVENT_BACKSPACE) {
            if (pos > 0) { pos--; terminal_print("\b \b"); }
            continue;
        }
        if (ev.type == INPUT_EVENT_CHAR && pos + 1U < max) {
            buf[pos++] = ev.character;
            terminal_put_char('*');
        }
    }
    buf[pos] = '\0';
    terminal_put_char('\n');
    return (int)pos;
}

static int cmd_passwd(int argc, const char* argv[], int background) {
    unsigned int target_uid;
    const ugdb_user_t* u;
    char old_pw[16];
    char new_pw[16];
    char confirm[16];
    (void)background;

    if (argc >= 2 && task_current_euid() == 0U) {
        u = ugdb_find_by_name(argv[1]);
        if (!u) { terminal_print_line("passwd: usuario no encontrado"); return 1; }
        target_uid = u->uid;
    } else {
        target_uid = task_current_euid();
        u = ugdb_find_by_uid(target_uid);
        if (!u) { terminal_print_line("passwd: usuario no encontrado"); return 1; }
    }

    if (task_current_euid() != 0U) {
        terminal_print("Contraseña actual: ");
        shell_read_secret_line(old_pw, sizeof(old_pw));
        if (!ugdb_check_password(target_uid, old_pw)) {
            terminal_print_line("passwd: contraseña incorrecta");
            return 1;
        }
    }

    terminal_print("Nueva contraseña: ");
    shell_read_secret_line(new_pw, sizeof(new_pw));
    terminal_print("Confirmar contraseña: ");
    shell_read_secret_line(confirm, sizeof(confirm));

    if (!str_equals(new_pw, confirm)) {
        terminal_print_line("passwd: las contraseñas no coinciden");
        return 1;
    }
    if (ugdb_set_password(target_uid, new_pw) < 0) {
        terminal_print_line("passwd: error al actualizar");
        return 1;
    }
    terminal_print("passwd: contraseña de '");
    terminal_print(u->name);
    terminal_print_line("' actualizada");
    return 1;
}

static int cmd_useradd(int argc, const char* argv[], int background) {
    unsigned int new_uid, new_gid;
    const char* name;
    char home[VFS_PATH_MAX];
    int i;
    (void)background;

    if (task_current_euid() != 0U) {
        terminal_print_line("[error] useradd: se requiere root"); return 1;
    }
    if (argc < 2) {
        terminal_print_line("Uso: useradd <nombre> [-u uid] [-g gid]"); return 1;
    }
    name = argv[1];
    if (ugdb_find_by_name(name)) {
        terminal_print("useradd: usuario ya existe: ");
        terminal_print_line(name); return 1;
    }
    new_uid = ugdb_next_uid();
    new_gid = ugdb_next_gid();
    for (i = 2; i < argc - 1; i++) {
        if (str_equals(argv[i], "-u") && i + 1 < argc) {
            new_uid = (unsigned int)parser_parse_integer(argv[++i], (int)new_uid);
        } else if (str_equals(argv[i], "-g") && i + 1 < argc) {
            new_gid = (unsigned int)parser_parse_integer(argv[++i], (int)new_gid);
        }
    }
    if (new_uid == 0xFFFFFFFFU) {
        terminal_print_line("useradd: tabla de usuarios llena"); return 1;
    }
    if (ugdb_find_by_uid(new_uid)) {
        terminal_print("useradd: uid "); terminal_print_uint(new_uid);
        terminal_print_line(" ya en uso"); return 1;
    }
    if (!ugdb_find_group_by_gid(new_gid))
        ugdb_add_group(new_gid, name);
    if (ugdb_add_user(new_uid, new_gid, name) < 0) {
        terminal_print_line("useradd: tabla de usuarios llena"); return 1;
    }
    shell_build_user_home_path(name, home, sizeof(home));
    shell_mkdir_p(home);
    vfs_chown(home, new_uid, new_gid);
    terminal_print("useradd: '"); terminal_print(name);
    terminal_print("' creado (uid="); terminal_print_uint(new_uid);
    terminal_print(", gid="); terminal_print_uint(new_gid);
    terminal_print_line(")");
    return 1;
}

static int cmd_userdel(int argc, const char* argv[], int background) {
    const ugdb_user_t* u;
    int remove_home = 0;
    int i;
    (void)background;

    if (task_current_euid() != 0U) {
        terminal_print_line("[error] userdel: se requiere root"); return 1;
    }
    if (argc < 2) { terminal_print_line("Uso: userdel [-r] <usuario>"); return 1; }
    for (i = 1; i < argc; i++) {
        if (str_equals(argv[i], "-r")) remove_home = 1;
    }
    u = ugdb_find_by_name(argv[argc - 1]);
    if (!u) {
        terminal_print("userdel: usuario no encontrado: ");
        terminal_print_line(argv[argc - 1]); return 1;
    }
    if (u->uid == 0U) { terminal_print_line("userdel: no se puede eliminar root"); return 1; }
    if (u->uid == task_current_euid()) {
        terminal_print_line("userdel: no puedes eliminarte a ti mismo"); return 1;
    }
    {
        char home[VFS_PATH_MAX];
        shell_build_user_home_path(u->name, home, sizeof(home));
        ugdb_del_user(u->uid);
        terminal_print("userdel: '"); terminal_print(argv[argc - 1]);
        terminal_print_line("' eliminado");
        if (remove_home) {
            vfs_delete(home);
            terminal_print("userdel: home "); terminal_print(home);
            terminal_print_line(" eliminado (best effort)");
        }
    }
    return 1;
}

static int cmd_usermod(int argc, const char* argv[], int background) {
    const ugdb_user_t* u;
    int i;
    (void)background;

    if (task_current_euid() != 0U) {
        terminal_print_line("[error] usermod: se requiere root"); return 1;
    }
    if (argc < 4) {
        terminal_print_line("Uso: usermod -n <nombre>|-g <gid>|-p <pass>  <usuario>"); return 1;
    }
    u = ugdb_find_by_name(argv[argc - 1]);
    if (!u) {
        terminal_print("usermod: usuario no encontrado: ");
        terminal_print_line(argv[argc - 1]); return 1;
    }
    for (i = 1; i < argc - 1; i++) {
        if (str_equals(argv[i], "-n") && i + 1 < argc - 1) {
            char old_home[VFS_PATH_MAX], new_home[VFS_PATH_MAX];
            shell_build_user_home_path(u->name,      old_home, sizeof(old_home));
            shell_build_user_home_path(argv[i + 1],  new_home, sizeof(new_home));
            ugdb_set_user_name(u->uid, argv[i + 1]);
            vfs_rename(old_home, new_home);
            i++;
        } else if (str_equals(argv[i], "-g") && i + 1 < argc - 1) {
            unsigned int ng = (unsigned int)parser_parse_integer(argv[i + 1], -1);
            if (!ugdb_find_group_by_gid(ng)) {
                terminal_print("usermod: grupo no encontrado: ");
                terminal_print_line(argv[i + 1]); return 1;
            }
            ugdb_set_user_gid(u->uid, ng);
            i++;
        } else if (str_equals(argv[i], "-p") && i + 1 < argc - 1) {
            ugdb_set_password(u->uid, argv[i + 1]);
            i++;
        }
    }
    terminal_print("usermod: '"); terminal_print(argv[argc - 1]);
    terminal_print_line("' actualizado");
    return 1;
}

static int cmd_groupadd(int argc, const char* argv[], int background) {
    unsigned int new_gid;
    (void)background;

    if (task_current_euid() != 0U) {
        terminal_print_line("[error] groupadd: se requiere root"); return 1;
    }
    if (argc < 2) { terminal_print_line("Uso: groupadd <nombre> [-g gid]"); return 1; }
    if (ugdb_find_group_by_name(argv[1])) {
        terminal_print("groupadd: grupo ya existe: ");
        terminal_print_line(argv[1]); return 1;
    }
    new_gid = ugdb_next_gid();
    if (argc >= 4 && str_equals(argv[2], "-g"))
        new_gid = (unsigned int)parser_parse_integer(argv[3], (int)new_gid);
    if (ugdb_find_group_by_gid(new_gid)) {
        terminal_print("groupadd: gid "); terminal_print_uint(new_gid);
        terminal_print_line(" ya en uso"); return 1;
    }
    if (ugdb_add_group(new_gid, argv[1]) < 0) {
        terminal_print_line("groupadd: tabla de grupos llena"); return 1;
    }
    terminal_print("groupadd: '"); terminal_print(argv[1]);
    terminal_print("' creado (gid="); terminal_print_uint(new_gid);
    terminal_print_line(")");
    return 1;
}

static int cmd_groupdel(int argc, const char* argv[], int background) {
    const ugdb_group_t* g;
    (void)background;

    if (task_current_euid() != 0U) {
        terminal_print_line("[error] groupdel: se requiere root"); return 1;
    }
    if (argc < 2) { terminal_print_line("Uso: groupdel <nombre>"); return 1; }
    g = ugdb_find_group_by_name(argv[1]);
    if (!g) {
        terminal_print("groupdel: grupo no encontrado: ");
        terminal_print_line(argv[1]); return 1;
    }
    if (ugdb_del_group(g->gid) < 0) {
        terminal_print("groupdel: no se puede eliminar '");
        terminal_print(argv[1]); terminal_print_line("'"); return 1;
    }
    terminal_print("groupdel: '"); terminal_print(argv[1]);
    terminal_print_line("' eliminado");
    return 1;
}

static int cmd_gpasswd(int argc, const char* argv[], int background) {
    const ugdb_group_t* g;
    const ugdb_user_t* u;
    int adding;
    (void)background;

    if (argc < 4 || (!str_equals(argv[1], "-a") && !str_equals(argv[1], "-d"))) {
        terminal_print_line("Uso: gpasswd -a|-d <usuario> <grupo>"); return 1;
    }
    if (task_current_euid() != 0U) {
        terminal_print_line("[error] gpasswd: se requiere root"); return 1;
    }
    adding = str_equals(argv[1], "-a");
    u = ugdb_find_by_name(argv[2]);
    if (!u) {
        terminal_print("gpasswd: usuario no encontrado: ");
        terminal_print_line(argv[2]); return 1;
    }
    g = ugdb_find_group_by_name(argv[3]);
    if (!g) {
        terminal_print("gpasswd: grupo no encontrado: ");
        terminal_print_line(argv[3]); return 1;
    }
    if (adding) {
        if (ugdb_group_add_member(g->gid, u->uid) < 0) {
            terminal_print_line("gpasswd: no se pudo añadir (¿tabla llena?)"); return 1;
        }
        terminal_print(u->name); terminal_print(" añadido al grupo ");
        terminal_print_line(g->name);
    } else {
        if (ugdb_group_remove_member(g->gid, u->uid) < 0) {
            terminal_print(u->name); terminal_print(" no era miembro de ");
            terminal_print_line(g->name); return 1;
        }
        terminal_print(u->name); terminal_print(" eliminado del grupo ");
        terminal_print_line(g->name);
    }
    terminal_print_line("(cambios efectivos en el siguiente login)");
    return 1;
}

static int cmd_login(int argc, const char* argv[], int background) {
    const ugdb_user_t* u;
    char pw[16];
    (void)background;

    if (argc < 2) { terminal_print_line("Uso: login <usuario>"); return 1; }
    u = ugdb_find_by_name(argv[1]);
    if (!u) {
        terminal_print("login: usuario desconocido: ");
        terminal_print_line(argv[1]); return 1;
    }
    /* Prompt only if the account has a password set */
    if (!ugdb_check_password(u->uid, "")) {
        terminal_print("Contraseña: ");
        shell_read_secret_line(pw, sizeof(pw));
        if (!ugdb_check_password(u->uid, pw)) {
            terminal_print_line("login: autenticación fallida"); return 1;
        }
    }
    /* Prepare home dir as root before identity switch so the user can write to it */
    {
        char pre_home[VFS_PATH_MAX];
        shell_build_user_home_path(u->name, pre_home, sizeof(pre_home));
        shell_mkdir_p(pre_home);
        vfs_chown(pre_home, u->uid, u->gid);
    }

    task_force_identity(u->uid, u->gid);
    shell_apply_user_session(0);
    terminal_print("Sesión iniciada como '"); terminal_print(u->name);
    terminal_print("' (uid="); terminal_print_uint(u->uid);
    terminal_print_line(")");
    return 1;
}

static int cmd_logout(int argc, const char* argv[], int background) {
    (void)argc; (void)argv; (void)background;
    if (task_current_euid() == UGDB_UID_ROOT) {
        terminal_print_line("logout: ya estás en la sesión root"); return 1;
    }
    task_force_identity(UGDB_UID_ROOT, UGDB_GID_WHEEL);
    shell_apply_user_session(0);
    terminal_print_line("Sesión cerrada. Sesión root restaurada.");
    return 1;
}

static int cmd_who(int argc, const char* argv[], int background) {
    (void)argc; (void)argv; (void)background;
    terminal_print(ugdb_username(task_current_euid()));
    terminal_print_line("     tty0");
    return 1;
}

static int cmd_users(int argc, const char* argv[], int background) {
    (void)argc; (void)argv; (void)background;
    terminal_print_line(ugdb_username(task_current_euid()));
    return 1;
}

/* ---- cmd_rmdir ---- */
static int cmd_rmdir(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    char entry[VFS_NAME_MAX];
    int fd;
    (void)background;
    if (argc < 2) { terminal_print_line("Uso: rmdir <ruta>"); return 1; }
    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_stat(path, &st) != 0) {
        terminal_print("[error] No existe: "); terminal_print_line(path); return 1;
    }
    if (!(st.flags & VFS_FLAG_DIR)) {
        terminal_print_line("[error] No es un directorio"); return 1;
    }
    fd = vfs_open_flags(path, VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd >= 0) {
        int has = (vfs_readdir(fd, 0, entry, sizeof(entry)) == 0);
        vfs_close(fd);
        if (has) { terminal_print_line("[error] El directorio no esta vacio"); return 1; }
    }
    if (vfs_delete(path) == 0)
        terminal_print_line("Directorio eliminado");
    else { terminal_print("[error] No se pudo eliminar: "); terminal_print_line(path); }
    return 1;
}

/* ---- cmd_find helpers ---- */
static void find_recursive(const char* dir, const char* pattern) {
    char entry[VFS_NAME_MAX];
    char full[VFS_PATH_MAX];
    int fd, idx = 0;
    unsigned int pi, ei;

    fd = vfs_open_flags(dir, VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd < 0) return;

    while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0) {
        vfs_stat_t st;
        idx++;
        /* build full path */
        for (pi = 0; dir[pi] && pi < VFS_PATH_MAX - 2U; pi++) full[pi] = dir[pi];
        if (pi > 0 && full[pi-1] != '/') full[pi++] = '/';
        for (ei = 0; entry[ei] && pi < VFS_PATH_MAX - 1U; ei++, pi++) full[pi] = entry[ei];
        full[pi] = '\0';

        /* match: if no pattern, print all; otherwise check if pattern is substring of name */
        if (!pattern || pattern[0] == '\0') {
            terminal_print_line(full);
        } else {
            /* simple substring match */
            unsigned int i, j, plen = str_length(pattern), elen = str_length(entry);
            int found = 0;
            for (i = 0; i + plen <= elen && !found; i++) {
                found = 1;
                for (j = 0; j < plen; j++)
                    if (entry[i+j] != pattern[j]) { found = 0; break; }
            }
            if (found) terminal_print_line(full);
        }

        if (vfs_stat(full, &st) == 0 && (st.flags & VFS_FLAG_DIR))
            find_recursive(full, pattern);
    }
    vfs_close(fd);
}

static int cmd_find(int argc, const char* argv[], int background) {
    char root[VFS_PATH_MAX];
    const char* pattern = 0;
    vfs_stat_t st;
    (void)background;
    /* find -name <pattern>  =>  search CWD */
    if (argc >= 3 && str_equals(argv[1], "-name")) {
        shell_resolve_path(".", root, sizeof(root));
        pattern = argv[2];
    } else if (argc >= 2) {
        shell_resolve_path(argv[1], root, sizeof(root));
        if (argc >= 4 && str_equals(argv[2], "-name")) pattern = argv[3];
    } else {
        shell_resolve_path(".", root, sizeof(root));
    }
    if (vfs_stat(root, &st) != 0 || !(st.flags & VFS_FLAG_DIR)) {
        terminal_print("[error] No existe o no es un directorio: ");
        terminal_print_line(root);
        return 1;
    }
    find_recursive(root, pattern);
    return 1;
}

/* ---- cmd_which ---- */
static int cmd_which(int argc, const char* argv[], int background) {
    int i;
    (void)background;
    if (argc < 2) { terminal_print_line("Uso: which <comando>"); return 1; }
    for (i = 0; i < command_count; i++) {
        if (str_equals(commands[i].name, argv[1])) {
            terminal_print("built-in: ");
            terminal_print_line(argv[1]);
            return 1;
        }
    }
    terminal_print(argv[1]); terminal_print_line(": no encontrado");
    return 1;
}

#define MORE_PAGE_LINES 20

static void shell_print_head_text(const char* text, int line_count) {
    int lines_printed = 0;
    unsigned int i = 0;

    if (text == 0 || line_count <= 0) {
        return;
    }

    while (text[i] != '\0' && lines_printed < line_count) {
        terminal_put_char(text[i]);
        if (text[i] == '\n') {
            lines_printed++;
        }
        i++;
    }

    if (i > 0 && text[i - 1] != '\n') {
        terminal_put_char('\n');
    }
}

static void shell_print_tail_text(const char* text, int line_count) {
    unsigned int total_lines = 0;
    unsigned int start = 0;
    unsigned int i;
    unsigned int length;

    if (text == 0 || line_count <= 0) {
        return;
    }

    length = str_length(text);

    for (i = 0; i < length; i++) {
        if (text[i] == '\n') {
            total_lines++;
        }
    }

    if (length > 0 && text[length - 1] != '\n') {
        total_lines++;
    }

    if (total_lines > (unsigned int)line_count) {
        unsigned int skip = total_lines - (unsigned int)line_count;

        for (i = 0; i < length && skip > 0; i++) {
            if (text[i] == '\n') {
                start = i + 1U;
                skip--;
            }
        }
    }

    for (i = start; i < length; i++) {
        terminal_put_char(text[i]);
    }

    if (start < length && text[length - 1] != '\n') {
        terminal_put_char('\n');
    }
}

static int shell_print_more_text(const char* text) {
    int line = 0;
    unsigned int i = 0;
    input_event_t ev;

    if (text == 0) {
        return 1;
    }

    while (text[i] != '\0') {
        terminal_put_char(text[i]);
        if (text[i] == '\n') {
            line++;
            if (line >= MORE_PAGE_LINES) {
                shell_print_text_with_color("-- Mas (q para salir, cualquier tecla para continuar) --", 0x0E);
                
                while (1) {
                    if (!input_poll_event(&ev)) { task_yield(); continue; }
                    if (ev.device_type != INPUT_DEVICE_KEYBOARD) continue;
                    if (ev.type == INPUT_EVENT_CHAR && (ev.character == 'q' || ev.character == 'Q')) {
                        terminal_put_char('\n');
                        return 1;
                    }
                    if (ev.type == INPUT_EVENT_CHAR || ev.type == INPUT_EVENT_ENTER) break;
                }
                terminal_put_char('\r');
                {
                    int k;
                    for (k = 0; k < 56; k++) terminal_put_char(' ');
                    terminal_put_char('\r');
                }
                line = 0;
            }
        }
        i++;
    }

    if (i > 0 && text[i - 1] != '\n') {
        terminal_put_char('\n');
    }

    return 1;
}

static void shell_count_text(const unsigned char* data,
                             unsigned int length,
                             unsigned int* lines,
                             unsigned int* words,
                             unsigned int* bytes) {
    int in_word = 0;
    unsigned int local_lines = 0;
    unsigned int local_words = 0;
    unsigned int local_bytes = 0;
    unsigned int i;

    if (data == 0) {
        if (lines) *lines = 0;
        if (words) *words = 0;
        if (bytes) *bytes = 0;
        return;
    }

    for (i = 0; i < length; i++) {
        local_bytes++;
        if (data[i] == '\n') local_lines++;
        if (data[i] == ' ' || data[i] == '\t' || data[i] == '\n' || data[i] == '\r') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            local_words++;
        }
    }

    if (lines) *lines = local_lines;
    if (words) *words = local_words;
    if (bytes) *bytes = local_bytes;
}

/* ---- cmd_head ---- */
static int cmd_head(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    char* text = 0;
    unsigned int text_length = 0;
    int n = 10, arg_idx = 1;
    (void)background;
    if (argc >= 3 && str_equals(argv[1], "-n")) {
        unsigned int i; n = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++)
            n = n * 10 + (argv[2][i] - '0');
        arg_idx = 3;
    } else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '1' && argv[1][1] <= '9') {
        /* head -5 file */
        unsigned int i; n = 0;
        for (i = 1; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
            n = n * 10 + (argv[1][i] - '0');
        arg_idx = 2;
    }
    if (arg_idx >= argc && !shell_has_pipe_input()) {
        terminal_print_line("Uso: head [-n N] <ruta>  o  comando | head [-n N]");
        return 1;
    }

    if (!shell_load_text_argument_or_pipe(arg_idx < argc ? argv[arg_idx] : 0, &text, &text_length, path, sizeof(path))) {
        return 1;
    }

    shell_print_head_text(text, n);
    kfree(text);
    return 1;
}

/* ---- cmd_tail ---- */
static int cmd_tail(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    char* text = 0;
    unsigned int text_length = 0;
    int n = 10, arg_idx = 1;
    (void)background;
    if (argc >= 3 && str_equals(argv[1], "-n")) {
        unsigned int i; n = 0;
        for (i = 0; argv[2][i] >= '0' && argv[2][i] <= '9'; i++)
            n = n * 10 + (argv[2][i] - '0');
        if (n == 0 && argv[2][0] != '0') { terminal_print_line("Uso: tail [-n N] <ruta>"); return 1; }
        arg_idx = 3;
    } else if (argc >= 2 && argv[1][0] == '-' && argv[1][1] >= '1' && argv[1][1] <= '9') {
        /* tail -3 file */
        unsigned int i; n = 0;
        for (i = 1; argv[1][i] >= '0' && argv[1][i] <= '9'; i++)
            n = n * 10 + (argv[1][i] - '0');
        arg_idx = 2;
    }
    if (arg_idx >= argc && !shell_has_pipe_input()) {
        terminal_print_line("Uso: tail [-n N] <ruta>  o  comando | tail [-n N]");
        return 1;
    }

    if (!shell_load_text_argument_or_pipe(arg_idx < argc ? argv[arg_idx] : 0, &text, &text_length, path, sizeof(path))) {
        return 1;
    }

    shell_print_tail_text(text, n);
    kfree(text);
    return 1;
}

/* ---- cmd_more ---- */
static int cmd_more(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    char* text = 0;
    unsigned int text_length = 0;
    (void)background;
    if (argc < 2 && !shell_has_pipe_input()) {
        terminal_print_line("Uso: more <ruta>  o  comando | more");
        return 1;
    }

    if (!shell_load_text_argument_or_pipe(argc >= 2 ? argv[1] : 0, &text, &text_length, path, sizeof(path))) {
        return 1;
    }

    shell_print_more_text(text);
    kfree(text);
    return 1;
}

/* ---- cmd_wc ---- */
static int cmd_wc(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    char* text = 0;
    unsigned int text_length = 0;
    int arg_idx = 1;
    int flag_l = 0, flag_w = 0, flag_c = 0;
    unsigned int lines = 0, words = 0, bytes = 0;
    int i;
    (void)background;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        const char* f = argv[arg_idx] + 1;
        for (i = 0; f[i]; i++) {
            if (f[i] == 'l') flag_l = 1;
            else if (f[i] == 'w') flag_w = 1;
            else if (f[i] == 'c') flag_c = 1;
        }
        arg_idx++;
    }
    if (!flag_l && !flag_w && !flag_c) flag_l = flag_w = flag_c = 1;
    if (arg_idx >= argc && !shell_has_pipe_input()) {
        terminal_print_line("Uso: wc [-l|-w|-c] <ruta>  o  comando | wc [-l|-w|-c]");
        return 1;
    }

    if (!shell_load_text_argument_or_pipe(arg_idx < argc ? argv[arg_idx] : 0, &text, &text_length, path, sizeof(path))) {
        return 1;
    }

    shell_count_text((const unsigned char*)text, text_length, &lines, &words, &bytes);
    kfree(text);

    if (flag_l) { terminal_print_uint(lines); terminal_put_char(' '); }
    if (flag_w) { terminal_print_uint(words); terminal_put_char(' '); }
    if (flag_c) { terminal_print_uint(bytes); terminal_put_char(' '); }
    if (arg_idx < argc) terminal_print_line(path);
    else terminal_put_char('\n');
    return 1;
}

static void shell_cmp_print_hex_byte(unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    char s[3];
    s[0] = hex[value >> 4];
    s[1] = hex[value & 0x0F];
    s[2] = '\0';
    terminal_print(s);
}

static void shell_cmp_print_octal_byte(unsigned char value) {
    char s[4];
    s[0] = (char)('0' + ((value >> 6) & 0x07));
    s[1] = (char)('0' + ((value >> 3) & 0x07));
    s[2] = (char)('0' + (value & 0x07));
    s[3] = '\0';
    terminal_print(s);
}

static void shell_cmp_print_byte_label(unsigned char value) {
    terminal_put_char('\'');
    switch (value) {
        case '\n': terminal_print("\\n"); break;
        case '\r': terminal_print("\\r"); break;
        case '\t': terminal_print("\\t"); break;
        case '\0': terminal_print("\\0"); break;
        case '\\': terminal_print("\\\\"); break;
        case '\'': terminal_print("\\'"); break;
        default:
            if (value >= 32U && value <= 126U)
                terminal_put_char((char)value);
            else {
                terminal_print("\\x");
                shell_cmp_print_hex_byte(value);
            }
            break;
    }
    terminal_put_char('\'');
}

static void shell_cmp_print_difference(unsigned int byte_index,
                                       unsigned char left,
                                       unsigned char right,
                                       int verbose) {
    terminal_print("Byte ");
    terminal_print_uint(byte_index);
    terminal_print(": ");
    if (verbose) {
        shell_cmp_print_byte_label(left);
        terminal_print(" (0x");
        shell_cmp_print_hex_byte(left);
        terminal_print(" / ");
        shell_cmp_print_octal_byte(left);
        terminal_print(") vs ");
        shell_cmp_print_byte_label(right);
        terminal_print(" (0x");
        shell_cmp_print_hex_byte(right);
        terminal_print(" / ");
        shell_cmp_print_octal_byte(right);
        terminal_print_line(")");
        return;
    }

    terminal_print("0x");
    shell_cmp_print_hex_byte(left);
    terminal_print(" vs 0x");
    shell_cmp_print_hex_byte(right);
    terminal_put_char('\n');
}

/* ---- cmd_cmp ---- */
static int cmd_cmp(int argc, const char* argv[], int background) {
    char pa[VFS_PATH_MAX], pb[VFS_PATH_MAX];
    int arg_idx = 1;
    int fda, fdb;
    unsigned char ba[1], bb[1];
    unsigned int offset = 0;
    unsigned int diff_count = 0;
    int list_all = 0;
    int verbose = 0;
    int quiet = 0;
    int different = 0;
    int ra, rb;
    (void)background;
    while (arg_idx < argc && argv[arg_idx][0] == '-') {
        int i;
        if (argv[arg_idx][1] == '\0') break;
        for (i = 1; argv[arg_idx][i] != '\0'; i++) {
            if (argv[arg_idx][i] == 'l') list_all = 1;
            else if (argv[arg_idx][i] == 'v') verbose = 1;
            else if (argv[arg_idx][i] == 'q') quiet = 1;
            else {
                terminal_print("[error] Opcion no soportada: ");
                terminal_print_line(argv[arg_idx]);
                terminal_print_line("Uso: cmp [-l] [-v] [-q] <archivo1> <archivo2>");
                return 1;
            }
        }
        arg_idx++;
    }
    if (argc - arg_idx < 2) { terminal_print_line("Uso: cmp [-l] [-v] [-q] <archivo1> <archivo2>"); return 1; }
    shell_resolve_path(argv[arg_idx], pa, sizeof(pa));
    shell_resolve_path(argv[arg_idx + 1], pb, sizeof(pb));
    fda = vfs_open_flags(pa, VFS_O_RDONLY);
    fdb = vfs_open_flags(pb, VFS_O_RDONLY);
    if (fda < 0) { terminal_print("[error] No encontrado: "); terminal_print_line(pa); if (fdb >= 0) vfs_close(fdb); return 1; }
    if (fdb < 0) { terminal_print("[error] No encontrado: "); terminal_print_line(pb); vfs_close(fda); return 1; }
    while (1) {
        ra = vfs_read(fda, ba, 1);
        rb = vfs_read(fdb, bb, 1);
        if (ra <= 0 && rb <= 0) {
            if (quiet)
                terminal_print_line(different ? "Son distintos" : "Son identicos");
            else if (diff_count == 0U)
                terminal_print_line("Los archivos son identicos");
            break;
        }
        if (ra <= 0 || rb <= 0) {
            different = 1;
            if (quiet) {
                terminal_print_line("Son distintos");
            } else if (list_all || verbose) {
                terminal_print("Longitud distinta a partir del byte ");
                terminal_print_uint(offset + 1U);
                terminal_put_char('\n');
            } else {
                terminal_print("Los archivos difieren en longitud (byte ");
                terminal_print_uint(offset); terminal_print_line(")");
            }
            break;
        }
        if (ba[0] != bb[0]) {
            diff_count++;
            different = 1;
            if (quiet) {
                terminal_print_line("Son distintos");
                break;
            }
            shell_cmp_print_difference((list_all || verbose) ? (offset + 1U) : offset,
                                       ba[0], bb[0], verbose);
            if (!list_all) break;
        }
        offset++;
    }
    vfs_close(fda); vfs_close(fdb);
    return 1;
}

/* ---- cmd_file ---- */
static int cmd_file(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    vfs_stat_t st;
    unsigned char hdr[8];
    int fd, n;
    (void)background;
    if (argc < 2) { terminal_print_line("Uso: file <ruta>"); return 1; }
    shell_resolve_path(argv[1], path, sizeof(path));
    if (vfs_stat(path, &st) != 0) { terminal_print("[error] No encontrado: "); terminal_print_line(path); return 1; }
    terminal_print(path); terminal_print(": ");
    if (st.flags & VFS_FLAG_DIR) { terminal_print_line("directorio"); return 1; }
    fd = vfs_open_flags(path, VFS_O_RDONLY);
    if (fd < 0) { terminal_print_line("no se pudo leer"); return 1; }
    n = vfs_read(fd, hdr, sizeof(hdr));
    vfs_close(fd);
    if (n >= 4 && hdr[0] == 0x7F && hdr[1] == 'E' && hdr[2] == 'L' && hdr[3] == 'F') {
        terminal_print_line("ELF executable");
    } else if (n >= 2 && hdr[0] == '#' && hdr[1] == '!') {
        terminal_print_line("script (shebang)");
    } else if (n >= 2 && hdr[0] == '#') {
        terminal_print_line("script de shell");
    } else {
        /* heuristic: printable? */
        int is_text = 1, i;
        for (i = 0; i < n; i++) {
            if (hdr[i] < 9 || (hdr[i] > 13 && hdr[i] < 32 && hdr[i] != 27)) { is_text = 0; break; }
        }
        terminal_print_line(is_text ? "texto ASCII" : "datos binarios");
    }
    return 1;
}

/* ---- cmd_du helpers ---- */
static unsigned int du_sum_dir(const char* dir) {
    char entry[VFS_NAME_MAX];
    char full[VFS_PATH_MAX];
    vfs_stat_t st;
    int fd, idx = 0;
    unsigned int total = 0;
    unsigned int pi, ei;

    fd = vfs_open_flags(dir, VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd < 0) return 0;

    while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0) {
        idx++;
        for (pi = 0; dir[pi] && pi < VFS_PATH_MAX - 2U; pi++) full[pi] = dir[pi];
        if (pi > 0 && full[pi-1] != '/') full[pi++] = '/';
        for (ei = 0; entry[ei] && pi < VFS_PATH_MAX - 1U; ei++, pi++) full[pi] = entry[ei];
        full[pi] = '\0';
        if (vfs_stat(full, &st) != 0) continue;
        if (st.flags & VFS_FLAG_DIR) {
            unsigned int sub = du_sum_dir(full);
            terminal_print_uint(sub / 1024U + 1U);
            terminal_print("\t");
            terminal_print_line(full);
            total += sub;
        } else {
            total += st.size;
        }
    }
    vfs_close(fd);
    return total;
}

static int cmd_du(int argc, const char* argv[], int background) {
    char path[VFS_PATH_MAX];
    unsigned int total;
    (void)background;
    if (argc < 2) shell_resolve_path(".", path, sizeof(path));
    else shell_resolve_path(argv[1], path, sizeof(path));
    total = du_sum_dir(path);
    terminal_print_uint(total / 1024U + 1U);
    terminal_print("\t");
    terminal_print_line(path);
    return 1;
}

/* ---- cmd_df ---- */
static int cmd_df(int argc, const char* argv[], int background) {
    int i, count;
    (void)argc; (void)argv; (void)background;
    terminal_print_line("Filesystem       Bloques  Usados   Libres   Uso%  Montaje");
    /* ramfs: always mounted at /, report as in-memory unlimited */
    terminal_print_line("ramfs            -        -        -        -     /");
    count = blkdev_count();
    for (i = 0; i < count; i++) {
        blkdev_t dev;
        if (blkdev_get(i, &dev) != 0) continue;
        if (!dev.used) continue;
        /* skip partitions if their parent is a whole disk already listed */
        {
            unsigned int total_kb = (dev.block_count / 2U); /* 512-byte blocks -> KB */
            char name_padded[18];
            unsigned int ni;
            for (ni = 0; dev.name[ni] && ni < 17U; ni++) name_padded[ni] = dev.name[ni];
            name_padded[ni++] = ' ';
            while (ni < 17U) name_padded[ni++] = ' ';
            name_padded[17] = '\0';
            terminal_print(name_padded);
            terminal_print_uint(total_kb);
            terminal_print("K");
            terminal_print_line("     -        -        -     (disco)");
        }
    }
    return 1;
}

/* ---- cmd_sync ---- */
static int cmd_sync(int argc, const char* argv[], int background) {
    int i, count;
    (void)argc; (void)argv; (void)background;
    count = blkdev_count();
    for (i = 0; i < count; i++) {
        blkdev_t dev;
        if (blkdev_get(i, &dev) == 0 && dev.used)
            blkdev_cache_invalidate_device(i);
    }
    terminal_print_line("sync: cache de bloques vaciada");
    return 1;
}

static int cmd_alarm(int argc, const char* argv[], int background) {
    unsigned int previous;
    unsigned int seconds = 0;

    (void)background;

    if (argc >= 2) {
        seconds = parse_positive_or_default(argv[1], 0);
    }

    previous = syscall_alarm(seconds);

    if (argc < 2 || seconds == 0) {
        terminal_print("alarm restante: ");
        terminal_print_uint(previous);
        terminal_print_line(" s");
        return 1;
    }

    terminal_print("alarm armada: ");
    terminal_print_uint(seconds);
    terminal_print(" s");
    if (previous != 0) {
        terminal_print(" (anterior restante: ");
        terminal_print_uint(previous);
        terminal_print(" s)");
    }
    terminal_print_line("");
    return 1;
}

/* ---- cmd_disk ---- */

/* Print a byte as exactly 2 hex digits. */
static void disk_print_byte(unsigned char b) {
    static const char hex[] = "0123456789ABCDEF";
    char s[3];
    s[0] = hex[(b >> 4) & 0xF];
    s[1] = hex[b & 0xF];
    s[2] = '\0';
    terminal_print(s);
}

/* Print a uint32 as exactly 4 hex digits (for LBA offsets). */
static void disk_print_hex4(unsigned int v) {
    disk_print_byte((unsigned char)(v >> 8));
    disk_print_byte((unsigned char)(v));
}

/* Print a uint32 as exactly 8 hex digits. */
static void disk_print_hex8(uint32_t v) {
    disk_print_byte((unsigned char)(v >> 24));
    disk_print_byte((unsigned char)(v >> 16));
    disk_print_byte((unsigned char)(v >> 8));
    disk_print_byte((unsigned char)(v));
}

/* Print a uint64 as exactly 16 hex digits. */
static void disk_print_hex16(uint64_t v) {
    disk_print_hex8((uint32_t)(v >> 32));
    disk_print_hex8((uint32_t)(v & 0xFFFFFFFFULL));
}

static uint32_t disk_le32(const unsigned char* p) {
    return ((uint32_t)p[0])
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t disk_le64(const unsigned char* p) {
    uint64_t lo = (uint64_t)disk_le32(p);
    uint64_t hi = (uint64_t)disk_le32(p + 4);
    return lo | (hi << 32);
}

static int disk_guid_is_zero(const unsigned char* g) {
    int i;
    for (i = 0; i < 16; i++) if (g[i] != 0U) return 0;
    return 1;
}

/* Print GUID in canonical text form. */
static void disk_print_guid(const unsigned char* g) {
    disk_print_byte(g[3]); disk_print_byte(g[2]); disk_print_byte(g[1]); disk_print_byte(g[0]);
    terminal_put_char('-');
    disk_print_byte(g[5]); disk_print_byte(g[4]);
    terminal_put_char('-');
    disk_print_byte(g[7]); disk_print_byte(g[6]);
    terminal_put_char('-');
    disk_print_byte(g[8]); disk_print_byte(g[9]);
    terminal_put_char('-');
    disk_print_byte(g[10]); disk_print_byte(g[11]); disk_print_byte(g[12]);
    disk_print_byte(g[13]); disk_print_byte(g[14]); disk_print_byte(g[15]);
}

/* Parse a decimal or 0x-prefixed hex number from a string.
   Returns the value; leaves *ok = 0 on invalid input. */
static unsigned int disk_parse_uint(const char* s, int* ok) {
    unsigned int result = 0;
    *ok = 0;
    if (!s || s[0] == '\0') return 0;

    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        /* hexadecimal */
        s += 2;
        if (s[0] == '\0') return 0;
        while (*s) {
            unsigned char c = (unsigned char)*s++;
            unsigned int digit;
            if (c >= '0' && c <= '9')      digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return 0;
            result = (result << 4) | digit;
        }
    } else {
        /* decimal */
        while (*s) {
            unsigned char c = (unsigned char)*s++;
            if (c < '0' || c > '9') return 0;
            result = result * 10 + (c - '0');
        }
    }
    *ok = 1;
    return result;
}

/* Hex dump of one 512-byte sector: 16 bytes per line, hex + ASCII. */
static void disk_hexdump(const unsigned char* buf, unsigned int size) {
    unsigned int i;
    for (i = 0; i < size; i += 16) {
        unsigned int j;
        unsigned int end = i + 16;
        if (end > size) end = size;

        /* Offset */
        disk_print_hex4(i);
        terminal_print(": ");

        /* Hex bytes */
        for (j = i; j < end; j++) {
            disk_print_byte(buf[j]);
            terminal_put_char(' ');
        }
        /* Padding if last line is short */
        for (j = end; j < i + 16; j++)
            terminal_print("   ");

        terminal_print(" |");

        /* ASCII */
        for (j = i; j < end; j++) {
            unsigned char c = buf[j];
            char s[2];
            s[0] = (c >= 0x20 && c < 0x7F) ? (char)c : '.';
            s[1] = '\0';
            terminal_print(s);
        }
        terminal_print_line("|");
    }
}

/* Return a short human-readable label for a common MBR partition type. */
static const char* disk_part_type_name(unsigned char type) {
    switch (type) {
        case 0x01: return "FAT12";
        case 0x04: return "FAT16<32M";
        case 0x05: return "Extended";
        case 0x06: return "FAT16";
        case 0x07: return "NTFS/exFAT";
        case 0x0B: return "FAT32";
        case 0x0C: return "FAT32 LBA";
        case 0x0E: return "FAT16 LBA";
        case 0x0F: return "Ext LBA";
        case 0x82: return "Linux swap";
        case 0x83: return "Linux";
        case 0xEE: return "GPT prot.";
        default:   return "unknown";
    }
}

static void disk_print_fsck_report(const char* fstype, const fat_fsck_report_t* r) {
    shell_print_text_with_color("fsck-lite ", 0x0B);
    shell_print_text_with_color(fstype, 0x0F);
    terminal_print_line(":");

    terminal_print("  errores: ");
    terminal_print_uint(r->errors);
    terminal_print("  warnings: ");
    terminal_print_uint(r->warnings);
    terminal_put_char('\n');

    terminal_print("  FAT fuera de rango: ");
    terminal_print_uint(r->fat_out_of_range);
    terminal_put_char('\n');

    terminal_print("  FAT bad cluster: ");
    terminal_print_uint(r->fat_bad_clusters);
    terminal_put_char('\n');

    terminal_print("  loops detectados: ");
    terminal_print_uint(r->fat_loops);
    terminal_put_char('\n');

    terminal_print("  entradas dir corruptas: ");
    terminal_print_uint(r->dir_corrupt_entries);
    terminal_put_char('\n');

    terminal_print("  first cluster invalido: ");
    terminal_print_uint(r->invalid_start_cluster);
    terminal_put_char('\n');

    terminal_print("  archivos revisados: ");
    terminal_print_uint(r->files_checked);
    terminal_print("  directorios revisados: ");
    terminal_print_uint(r->dirs_checked);
    terminal_put_char('\n');
}

static int cmd_disk(int argc, const char* argv[], int background) {
    (void)background;

    /* disk   → list all registered block devices */
    if (argc == 1) {
        int total = blkdev_count();
        int i;

        shell_print_text_with_color("Dispositivos de bloque\n", 0x0B);

        if (total == 0) {
            terminal_print_line("  (sin dispositivos detectados)");
            return 1;
        }

        for (i = 0; i < BLKDEV_MAX; i++) {
            blkdev_t dev;
            unsigned int mb;

            if (blkdev_get(i, &dev) < 0) continue;

            /* index + name */
            shell_print_text_with_color("  [", 0x07);
            terminal_print_uint((unsigned int)i);
            shell_print_text_with_color("] ", 0x07);
            shell_print_text_with_color(dev.name, 0x0F);
            terminal_print("  ");

            /* size */
            mb = dev.block_count / 2048;   /* 512-byte sectors → MiB */
            terminal_print_uint(mb);
            terminal_print(" MB  (");
            terminal_print_uint(dev.block_count);
            terminal_print(" sectores)");

            /* partition type for child devices */
            if (dev.part_type != 0) {
                terminal_print("  tipo 0x");
                disk_print_byte(dev.part_type);
                terminal_print(" (");
                terminal_print(disk_part_type_name(dev.part_type));
                terminal_put_char(')');
            }

            terminal_print_line("");
        }
        return 1;
    }

    /* disk read <lba> [name]   → hex dump one 512-byte sector */
    if (str_equals_ignore_case(argv[1], "read")) {
        unsigned char sector[512];
        unsigned int  lba;
        int           ok;
        int           dev_idx;
        int           result;
        const char*   devname = "hd0";

        if (argc < 3) {
            terminal_print_line("Uso: disk read <lba> [nombre]");
            return 1;
        }

        lba = disk_parse_uint(argv[2], &ok);
        if (!ok) {
            terminal_print("[error] LBA invalido: ");
            terminal_print_line(argv[2]);
            return 1;
        }

        if (argc >= 4) devname = argv[3];

        dev_idx = blkdev_find(devname);
        if (dev_idx < 0) {
            terminal_print("[error] dispositivo no encontrado: ");
            terminal_print_line(devname);
            return 1;
        }

        result = blkdev_read(dev_idx, (uint32_t)lba, 1, sector);
        if (result != 1) {
            terminal_print_line("[error] fallo de lectura");
            return 1;
        }

        shell_print_text_with_color("Sector LBA ", 0x0B);
        terminal_print_uint(lba);
        shell_print_text_with_color(" (", 0x0B);
        terminal_print(devname);
        shell_print_text_with_color(")\n", 0x0B);
        disk_hexdump(sector, 512);
        return 1;
    }

    /* disk mount <devname> <path>   → mount FAT16 device at VFS path */
    if (str_equals_ignore_case(argv[1], "mount")) {
        vfs_node_t* fat_root;
        int         dev_idx;
        const char* devname;
        const char* mntpath;

        if (argc < 4) {
            terminal_print_line("Uso: disk mount <dispositivo> <ruta>");
            terminal_print_line("  Ej: disk mount hd0p1 /fat");
            return 1;
        }

        devname = argv[2];
        mntpath = argv[3];

        dev_idx = blkdev_find(devname);
        if (dev_idx < 0) {
            terminal_print("[error] dispositivo no encontrado: ");
            terminal_print_line(devname);
            return 1;
        }

        fat_root = fat16_mount(dev_idx);
        if (!fat_root) fat_root = fat32_mount(dev_idx);
        if (!fat_root) {
            terminal_print_line("[error] no es FAT16/FAT32 o BPB invalido");
            return 1;
        }

        if (vfs_mount(mntpath, fat_root) < 0) {
            terminal_print_line("[error] tabla de montajes llena");
            return 1;
        }

        shell_print_text_with_color(devname, 0x0B);
        terminal_print(" montado en ");
        shell_print_text_with_color(mntpath, 0x0F);
        terminal_put_char('\n');
        return 1;
    }

    /* disk fsck <devname>   → lightweight FAT integrity scan */
    if (str_equals_ignore_case(argv[1], "fsck")) {
        const char*        devname;
        int                dev_idx;
        fat_fsck_report_t  rep;
        int                rc;

        if (argc < 3) {
            terminal_print_line("Uso: disk fsck <dispositivo>");
            terminal_print_line("  Ej: disk fsck hd0p1");
            return 1;
        }

        devname = argv[2];
        dev_idx = blkdev_find(devname);
        if (dev_idx < 0) {
            terminal_print("[error] dispositivo no encontrado: ");
            terminal_print_line(devname);
            return 1;
        }

        rc = fat16_fsck_lite(dev_idx, &rep);
        if (rc == 0) {
            disk_print_fsck_report("FAT16", &rep);
            return 1;
        }

        rc = fat32_fsck_lite(dev_idx, &rep);
        if (rc == 0) {
            disk_print_fsck_report("FAT32", &rep);
            return 1;
        }

        terminal_print_line("[error] no es FAT16/FAT32 o BPB invalido");
        return 1;
    }

    /* disk gpt <devname>   → inspect GPT header + partition entries */
    if (str_equals_ignore_case(argv[1], "gpt")) {
        const char*    devname;
        int            dev_idx;
        blkdev_t       dev;
        unsigned char  sec[512];
        uint64_t       table_lba;
        uint32_t       entry_count;
        uint32_t       entry_size;
        uint32_t       shown = 0;
        uint32_t       i;

        if (argc < 3) {
            terminal_print_line("Uso: disk gpt <dispositivo>");
            terminal_print_line("  Ej: disk gpt hd0");
            return 1;
        }

        devname = argv[2];
        dev_idx = blkdev_find(devname);
        if (dev_idx < 0) {
            terminal_print("[error] dispositivo no encontrado: ");
            terminal_print_line(devname);
            return 1;
        }

        if (blkdev_get(dev_idx, &dev) < 0) {
            terminal_print_line("[error] no se pudo leer descriptor del dispositivo");
            return 1;
        }

        if (dev.block_size != 512U) {
            terminal_print_line("[error] GPT requiere block size de 512 bytes");
            return 1;
        }

        if (blkdev_read(dev_idx, 1U, 1U, sec) != 1) {
            terminal_print_line("[error] fallo leyendo GPT header (LBA1)");
            return 1;
        }

        if (!(sec[0] == 'E' && sec[1] == 'F' && sec[2] == 'I' && sec[3] == ' ' &&
              sec[4] == 'P' && sec[5] == 'A' && sec[6] == 'R' && sec[7] == 'T')) {
            terminal_print_line("[error] firma GPT no encontrada (EFI PART)");
            return 1;
        }

        table_lba   = disk_le64(sec + 72);
        entry_count = disk_le32(sec + 80);
        entry_size  = disk_le32(sec + 84);

        shell_print_text_with_color("GPT header ", 0x0B);
        shell_print_text_with_color(devname, 0x0F);
        terminal_put_char('\n');

        terminal_print("  revision: 0x");
        disk_print_hex8(disk_le32(sec + 8));
        terminal_print("  header_size: ");
        terminal_print_uint(disk_le32(sec + 12));
        terminal_put_char('\n');

        terminal_print("  current_lba: 0x");
        disk_print_hex16(disk_le64(sec + 24));
        terminal_print("  backup_lba: 0x");
        disk_print_hex16(disk_le64(sec + 32));
        terminal_put_char('\n');

        terminal_print("  first_usable: 0x");
        disk_print_hex16(disk_le64(sec + 40));
        terminal_print("  last_usable: 0x");
        disk_print_hex16(disk_le64(sec + 48));
        terminal_put_char('\n');

        terminal_print("  entries_lba: 0x");
        disk_print_hex16(table_lba);
        terminal_print("  entries: ");
        terminal_print_uint(entry_count);
        terminal_print("  entry_size: ");
        terminal_print_uint(entry_size);
        terminal_put_char('\n');

        if (entry_size < 128U || entry_size > 512U) {
            terminal_print_line("[error] tamaño de entrada GPT no soportado");
            return 1;
        }

        for (i = 0; i < entry_count; i++) {
            unsigned char entry[512];
            uint64_t      byte_off = (uint64_t)i * (uint64_t)entry_size;
            uint64_t      lba      = table_lba + (byte_off / 512ULL);
            uint32_t      off      = (uint32_t)(byte_off % 512ULL);
            uint32_t      j;
            uint64_t      first_lba;
            uint64_t      last_lba;
            uint64_t      sectors;

            if (lba >= (uint64_t)dev.block_count) break;

            if (off + entry_size <= 512U) {
                if (blkdev_read(dev_idx, (uint32_t)lba, 1U, sec) != 1) break;
                for (j = 0; j < entry_size; j++) entry[j] = sec[off + j];
            } else {
                uint32_t k = 0;
                if (blkdev_read(dev_idx, (uint32_t)lba, 1U, sec) != 1) break;
                for (j = off; j < 512U && k < entry_size; j++) entry[k++] = sec[j];
                if (blkdev_read(dev_idx, (uint32_t)(lba + 1ULL), 1U, sec) != 1) break;
                for (j = 0; j < 512U && k < entry_size; j++) entry[k++] = sec[j];
            }

            if (disk_guid_is_zero(entry + 0)) continue;

            first_lba = disk_le64(entry + 32);
            last_lba  = disk_le64(entry + 40);
            if (last_lba < first_lba) continue;
            sectors = last_lba - first_lba + 1ULL;

            terminal_print("  p");
            terminal_print_uint(i + 1U);
            terminal_print(": type=");
            disk_print_guid(entry + 0);
            terminal_print("  first=0x");
            disk_print_hex16(first_lba);
            terminal_print("  last=0x");
            disk_print_hex16(last_lba);
            terminal_print("  sectors=");
            if (sectors <= 0xFFFFFFFFULL) {
                terminal_print_uint((unsigned int)sectors);
            } else {
                terminal_print("0x");
                disk_print_hex16(sectors);
            }
            terminal_put_char('\n');
            shown++;
        }

        terminal_print("  entradas usadas detectadas: ");
        terminal_print_uint(shown);
        terminal_put_char('\n');
        return 1;
    }

    terminal_print_line("Uso: disk [read <lba> [nombre]] [mount <dev> <ruta>] [fsck <dev>] [gpt <dev>]");
    return 1;
}

/* Static storage for up to 8 path completions */
static char completion_store[8][VFS_PATH_MAX];

int shell_complete_path(const char* prefix, const char* matches[], int max_matches) {
    char dir_part[VFS_PATH_MAX];
    char name_part[VFS_NAME_MAX];
    char search_path[VFS_PATH_MAX];
    char entry[VFS_NAME_MAX];
    int  found = 0;
    int  fd, idx;
    int  last_slash = -1;
    unsigned int i;

    if (!prefix) return 0;
    if (max_matches <= 0) return 0;
    if (max_matches > 8) max_matches = 8;

    /* Split prefix into directory part and name part at last '/' */
    for (i = 0; prefix[i]; i++) {
        if (prefix[i] == '/') last_slash = (int)i;
    }

    if (last_slash < 0) {
        /* No slash: no dir_part, search in cwd */
        dir_part[0] = '\0';
        for (i = 0; prefix[i] && i < VFS_NAME_MAX - 1U; i++) name_part[i] = prefix[i];
        name_part[i] = '\0';
    } else {
        /* dir_part = everything up to and including the last '/' */
        for (i = 0; i <= (unsigned int)last_slash && i < VFS_PATH_MAX - 1U; i++)
            dir_part[i] = prefix[i];
        dir_part[i] = '\0';
        /* name_part = rest */
        const char* tail = prefix + last_slash + 1;
        for (i = 0; tail[i] && i < VFS_NAME_MAX - 1U; i++) name_part[i] = tail[i];
        name_part[i] = '\0';
    }

    /* Resolve dir_part against cwd to get search_path */
    if (dir_part[0] == '\0') {
        /* search in cwd */
        for (i = 0; shell_cwd[i] && i < VFS_PATH_MAX - 1U; i++) search_path[i] = shell_cwd[i];
        search_path[i] = '\0';
    } else {
        shell_resolve_path(dir_part, search_path, sizeof(search_path));
    }

    /* Open the directory via VFS */
    fd = vfs_open_flags(search_path, VFS_O_RDONLY | VFS_O_DIRECTORY);
    if (fd < 0) return 0;

    idx = 0;
    while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0 && found < max_matches) {
        if (str_starts_with_ignore_case(entry, name_part)) {
            /* Build full token: dir_part + entry */
            unsigned int j = 0, k;
            for (k = 0; dir_part[k] && j < VFS_PATH_MAX - 1U; k++) completion_store[found][j++] = dir_part[k];
            for (k = 0; entry[k]   && j < VFS_PATH_MAX - 1U; k++) completion_store[found][j++] = entry[k];
            completion_store[found][j] = '\0';
            matches[found] = completion_store[found];
            found++;
        }
        idx++;
    }

    vfs_close(fd);
    return found;
}