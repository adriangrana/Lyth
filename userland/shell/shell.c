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

#define SHELL_MAX_ARGS 8
#define SHELL_TOKEN_MAX 64
#define SHELL_ENV_MAX 16
#define SHELL_ENV_NAME_MAX 16
#define SHELL_ENV_VALUE_MAX 64
#define SHELL_PIPE_MAX 1024
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
static int cmd_ps(int argc, const char* argv[], int background);
static int cmd_kill(int argc, const char* argv[], int background);
static int cmd_nice(int argc, const char* argv[], int background);
static int cmd_task(int argc, const char* argv[], int background);
static int cmd_mem(int argc, const char* argv[], int background);
static int cmd_wait(int argc, const char* argv[], int background);
static int cmd_signal(int argc, const char* argv[], int background);
static int cmd_mouse(int argc, const char* argv[], int background);
static int cmd_dmesg(int argc, const char* argv[], int background);
static int cmd_gfxdemo(int argc, const char* argv[], int background);
static int cmd_ls(int argc, const char* argv[], int background);
static int cmd_cat(int argc, const char* argv[], int background);
static int cmd_source(int argc, const char* argv[], int background);
static int cmd_elfinfo(int argc, const char* argv[], int background);
static int cmd_exec(int argc, const char* argv[], int background);
static int cmd_yield(int argc, const char* argv[], int background);
static int cmd_vfs(int argc, const char* argv[], int background);
static int cmd_disk(int argc, const char* argv[], int background);
static int cmd_cd(int argc, const char* argv[], int background);
static int cmd_pwd(int argc, const char* argv[], int background);
static int cmd_touch(int argc, const char* argv[], int background);
static int cmd_rm(int argc, const char* argv[], int background);
static int cmd_mkdir(int argc, const char* argv[], int background);
static int cmd_getpid(int argc, const char* argv[], int background);
static void shell_resolve_path(const char* input, char* out, unsigned int out_size);

/* Current working directory (always an absolute VFS path). */
static char shell_cwd[VFS_PATH_MAX];

const char* shell_get_cwd(void) { return shell_cwd; }

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
    {"ps",      cmd_ps,      "lista tareas activas"},
    {"jobs",    cmd_ps,      "alias de ps"},
    {"kill",    cmd_kill,    "cancela una tarea: kill <id>"},
    {"nice",    cmd_nice,    "cambia prioridad: nice <id> <high|normal|low>"},
    {"task",    cmd_task,    "muestra la tarea actual y foreground"},
    {"mem",     cmd_mem,     "estadisticas de heap, memoria fisica y paging"},
    {"wait",    cmd_wait,    "bloquea una tarea en evento: wait <id> [&]"},
    {"signal",  cmd_signal,  "despierta tareas bloqueadas: signal <id>"},
    {"mouse",   cmd_mouse,   "muestra estado del raton PS/2"},
    {"dmesg",   cmd_dmesg,   "muestra o limpia logs del kernel: dmesg [clear]"},
    {"gfxdemo", cmd_gfxdemo, "dibuja primitivas graficas en framebuffer"},
    {"ls",      cmd_ls,      "lista directorio VFS: ls [ruta]"},
    {"cat",     cmd_cat,     "muestra un archivo VFS: cat <ruta>"},
    {"source",  cmd_source,  "ejecuta script del FS: source <NOMBRE>"},
    {"elfinfo", cmd_elfinfo, "inspecciona un ELF del FS: elfinfo <NOMBRE>"},
    {"exec",    cmd_exec,    "carga y ejecuta un ELF: exec <NOMBRE> [args...] [&]"},
    {"getpid",  cmd_getpid,  "muestra el PID del proceso actual"},
    {"yield",   cmd_yield,   "cede CPU al scheduler"},
    {"vfs",     cmd_vfs,     "VFS: mounts, ls/cat/touch/rm: vfs [ls|cat|touch|rm] [ruta]"},
    {"disk",    cmd_disk,    "Bloques: disk [read <lba> [dev]] [mount <dev> <ruta>]"},
    {"cd",      cmd_cd,      "cambia directorio: cd [ruta]"},
    {"pwd",     cmd_pwd,     "muestra el directorio actual"},
    {"touch",   cmd_touch,   "crea archivo vacio (sin contenido): touch <ruta>"},
    {"rm",      cmd_rm,      "elimina archivo: rm <ruta>"},
    {"mkdir",   cmd_mkdir,   "crea directorio: mkdir <ruta>"},
};

static const int command_count = sizeof(commands) / sizeof(commands[0]);
static shell_env_var_t shell_env[SHELL_ENV_MAX];
static char shell_pipe_buffer[SHELL_PIPE_MAX];
static unsigned int shell_pipe_length = 0;
static shell_redir_file_t shell_redir_files[SHELL_REDIR_MAX];
static const char* current_theme_name = "default";
static unsigned char current_theme_banner_color = 0x0B;
static unsigned char current_theme_hint_color = 0x07;

static void shell_apply_theme(const char* name, int print_feedback);
static int shell_find_unquoted_char(const char* text, char target);
static void shell_trim_trailing_spaces(char* text);
static void shell_set_pipe_input(const char* text);
static void shell_clear_pipe_input(void);
static int shell_has_pipe_input(void);
static int shell_redir_find(const char* name);
static int shell_redir_store(const char* name, const char* content, unsigned int length, int append);
static int shell_read_text_source(const char* name, char* buffer, unsigned int buffer_size);
static int shell_execute_line_raw(const char* line);

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
    shell_print_text_with_color("Lyth OS shell\n", current_theme_banner_color);
    shell_print_text_with_color("-------------\n", current_theme_banner_color);
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

static void shell_set_pipe_input(const char* text) {
    unsigned int length;

    shell_pipe_buffer[0] = '\0';
    shell_pipe_length = 0;

    if (text == 0) {
        return;
    }

    length = str_length(text);
    if (length >= SHELL_PIPE_MAX) {
        length = SHELL_PIPE_MAX - 1;
    }

    for (unsigned int i = 0; i < length; i++) {
        shell_pipe_buffer[i] = text[i];
    }

    shell_pipe_buffer[length] = '\0';
    shell_pipe_length = length;
}

static void shell_clear_pipe_input(void) {
    shell_pipe_buffer[0] = '\0';
    shell_pipe_length = 0;
}

static int shell_has_pipe_input(void) {
    return shell_pipe_length > 0 && shell_pipe_buffer[0] != '\0';
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

    if (name == 0 || name[0] == '\0' || content == 0) {
        return 0;
    }

    /* Resolve the output filename against cwd so "test.txt" becomes "/cwd/test.txt" */
    shell_resolve_path(name, vfs_path, sizeof(vfs_path));

    /* Create the file if it doesn't exist yet */
    vfs_node_t* existing = vfs_resolve(vfs_path);
    if (!existing) {
        if (vfs_create(vfs_path, VFS_FLAG_FILE) != 0)
            return 0;
    } else {
        if (existing->flags & VFS_FLAG_DYNAMIC) kfree(existing);
    }

    /* Open and write */
    fd = vfs_open(vfs_path);
    if (fd < 0) return 0;

    if (append) {
        /* seek to end for >> */
        vfs_seek(fd, 0, VFS_SEEK_END);
    }
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
    terminal_print_line("Lyth OS v0.4");
    terminal_print_line("Kernel hobby en C + ASM");
    terminal_print_line("PIT, scheduler, heap, syscalls y FS en memoria");
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

    (void)argc;
    (void)argv;
    (void)background;

    ticks = syscall_get_ticks();
    ms = timer_ticks_to_ms(ticks);
    terminal_print("Ticks: ");
    terminal_print_uint(ticks);
    terminal_put_char('\n');
    terminal_print("Uptime ms: ");
    terminal_print_uint(ms);
    terminal_put_char('\n');
    return 1;
}

static int cmd_ps(int argc, const char* argv[], int background) {
    task_snapshot_t snapshots[8];
    int count;

    (void)argc;
    (void)argv;
    (void)background;

    count = task_list(snapshots, 8);
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

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: kill <id>");
        return 1;
    }

    id = parse_positive_or_default(argv[1], 0);
    if (id == 0 || !task_kill((int)id)) {
        terminal_print_line("No existe esa tarea");
        return 1;
    }

    terminal_print("Tarea cancelada: ");
    terminal_print_uint(id);
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

    fd = vfs_open(path);
    if (fd < 0) {
        terminal_print("[error] no se pudo abrir: ");
        terminal_print_line(path);
        return 1;
    }

    while (vfs_readdir(fd, (unsigned int)idx, entry, sizeof(entry)) == 0) {
        /* Try to open the entry to detect if it's a directory */
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
        if (n) fl = n->flags;

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

    fd = vfs_open(path);
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

    if (argc < 2) {
        terminal_print_line("Uso: exec <NOMBRE> [args...] [&]");
        return 1;
    }

    /* argv[1] = path; forward argv[1..] as the ELF's argv */
    id = usermode_spawn_elf_vfs_argv(argv[1],
                                     argc - 1, (const char* const*)(argv + 1),
                                     0, 0,
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

static int cmd_getpid(int argc, const char* argv[], int background) {
    (void)argc;
    (void)argv;
    (void)background;
    terminal_print("PID: ");
    terminal_print_uint((unsigned int)task_current_id());
    terminal_put_char('\n');
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
    shell_env_set("OS", "Lyth");
    shell_env_set("ARCH", "i386");
    shell_apply_theme("default", 0);
    shell_print_banner();
}

static int shell_execute_line_raw(const char* line) {
    char tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char expanded_tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char filtered_tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    char input_text[SHELL_PIPE_MAX];
    char captured_output[SHELL_PIPE_MAX];
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
        int  in_fd, in_n, in_total = 0;
        shell_resolve_path(input_redirection, in_path, sizeof(in_path));
        in_fd = vfs_open(in_path);
        if (in_fd < 0) {
            terminal_print("No existe la entrada para redireccion: ");
            terminal_print_line(in_path);
            return 1;
        }
        while ((in_n = vfs_read(in_fd, (unsigned char*)input_text + in_total,
                                (unsigned int)(sizeof(input_text) - 1 - (unsigned int)in_total))) > 0)
            in_total += in_n;
        input_text[in_total] = '\0';
        vfs_close(in_fd);
        shell_set_pipe_input(input_text);
    }

    if (output_redirect) {
        capture_active = 1;
        terminal_capture_begin(captured_output, sizeof(captured_output));
    }

    for (int i = 0; i < command_count; i++) {
        if (command_name_matches(filtered_argv[0], commands[i].name)) {
            int result = commands[i].fn(filtered_argc, filtered_argv, background);

            if (capture_active) {
                capture_length = (int)terminal_capture_end();
                shell_redir_store(output_redirection, captured_output, (unsigned int)capture_length, append_redirect);
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
        capture_length = (int)terminal_capture_end();
        shell_redir_store(output_redirection, captured_output, (unsigned int)capture_length, append_redirect);
    }

    return 1;
}

int shell_execute_line(const char* line) {
    char left_line[SHELL_PIPE_MAX];
    char right_line[SHELL_PIPE_MAX];
    char captured_output[SHELL_PIPE_MAX];
    int pipe_index;
    int right_index;
    int result;

    if (line == 0) {
        return 1;
    }

    pipe_index = shell_find_unquoted_char(line, '|');
    if (pipe_index < 0) {
        return shell_execute_line_raw(line);
    }

    if (shell_find_unquoted_char(line + pipe_index + 1, '|') >= 0) {
        terminal_print_line("Solo se soporta un pipe por ahora");
        return 1;
    }

    if (pipe_index <= 0 || pipe_index >= (int)sizeof(left_line)) {
        terminal_print_line("Uso: comando1 | comando2");
        return 1;
    }

    for (int i = 0; i < pipe_index && i < (int)sizeof(left_line) - 1; i++) {
        left_line[i] = line[i];
    }
    left_line[pipe_index < (int)sizeof(left_line) ? pipe_index : (int)sizeof(left_line) - 1] = '\0';
    shell_trim_trailing_spaces(left_line);

    if (shell_find_unquoted_char(left_line, '>') >= 0) {
        terminal_print_line("No se admite redireccion de salida en el lado izquierdo de un pipe");
        return 1;
    }

    const char* right_start = line + pipe_index + 1;
    while (*right_start == ' ' || *right_start == '\t') {
        right_start++;
    }

    right_index = 0;
    while (right_start[right_index] != '\0' && right_index < (int)sizeof(right_line) - 1) {
        right_line[right_index] = right_start[right_index];
        right_index++;
    }
    right_line[right_index] = '\0';

    if (right_line[0] == '\0') {
        terminal_print_line("Uso: comando1 | comando2");
        return 1;
    }

    terminal_capture_begin(captured_output, sizeof(captured_output));
    shell_execute_line_raw(left_line);
    terminal_capture_end();

    shell_set_pipe_input(captured_output);
    result = shell_execute_line_raw(right_line);
    shell_clear_pipe_input();
    return result;
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

    (void)background;

    if (argc < 2) {
        /* cd with no args → go to root */
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
        int fd = vfs_open("/");
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

        fd = vfs_open(path);
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

        int fd = vfs_open(path);
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

    terminal_print_line("Uso: vfs [ls [ruta] | cat <ruta> | touch <ruta> | rm <ruta>]");
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
    if (vfs_delete(path) == 0) {
        shell_print_text_with_color("Eliminado: ", 0x0C);
        terminal_print_line(path);
    } else {
        terminal_print("[error] No se pudo eliminar: ");
        terminal_print_line(path);
    }
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
        if (!fat_root) {
            terminal_print_line("[error] no es FAT16 o BPB invalido");
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

    terminal_print_line("Uso: disk [read <lba> [nombre]] [mount <dev> <ruta>]");
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
    fd = vfs_open(search_path);
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