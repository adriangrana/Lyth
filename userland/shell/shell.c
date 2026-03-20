#include "shell.h"
#include "terminal.h"
#include "string.h"
#include "shell_input.h"
#include "parser.h"
#include "task.h"
#include "timer.h"
#include "heap.h"
#include "syscall.h"
#include "physmem.h"
#include "paging.h"

#define SHELL_MAX_ARGS 8
#define SHELL_TOKEN_MAX 64

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

static int cmd_help(int argc, const char* argv[], int background);
static int cmd_clear(int argc, const char* argv[], int background);
static int cmd_about(int argc, const char* argv[], int background);
static int cmd_echo(int argc, const char* argv[], int background);
static int cmd_color(int argc, const char* argv[], int background);
static int cmd_history(int argc, const char* argv[], int background);
static int cmd_count(int argc, const char* argv[], int background);
static int cmd_sleep(int argc, const char* argv[], int background);
static int cmd_uptime(int argc, const char* argv[], int background);
static int cmd_ps(int argc, const char* argv[], int background);
static int cmd_kill(int argc, const char* argv[], int background);
static int cmd_task(int argc, const char* argv[], int background);
static int cmd_mem(int argc, const char* argv[], int background);
static int cmd_ls(int argc, const char* argv[], int background);
static int cmd_cat(int argc, const char* argv[], int background);
static int cmd_yield(int argc, const char* argv[], int background);

static command_t commands[] = {
    {"help",    cmd_help,    "muestra esta ayuda"},
    {"clear",   cmd_clear,   "limpia la pantalla"},
    {"cls",     cmd_clear,   "alias de clear"},
    {"about",   cmd_about,   "informacion del sistema"},
    {"echo",    cmd_echo,    "muestra texto (soporta comillas)"},
    {"color",   cmd_color,   "cambia color: color green|red|white|blue"},
    {"history", cmd_history, "historial gestionado por la capa de entrada"},
    {"count",   cmd_count,   "demo programada; soporta '&' y Ctrl+C"},
    {"sleep",   cmd_sleep,   "duerme en ms; soporta '&'"},
    {"uptime",  cmd_uptime,  "muestra ticks y tiempo desde arranque"},
    {"ps",      cmd_ps,      "lista tareas activas"},
    {"jobs",    cmd_ps,      "alias de ps"},
    {"kill",    cmd_kill,    "cancela una tarea: kill <id>"},
    {"task",    cmd_task,    "muestra la tarea actual y foreground"},
    {"mem",     cmd_mem,     "estadisticas de heap, memoria fisica y paging"},
    {"ls",      cmd_ls,      "lista archivos del FS en memoria"},
    {"cat",     cmd_cat,     "muestra un archivo: cat <NOMBRE>"},
    {"yield",   cmd_yield,   "cede CPU al scheduler"},
};

static const int command_count = sizeof(commands) / sizeof(commands[0]);

static void shell_print_banner(void) {
    terminal_print_line("Lyth OS shell");
    terminal_print_line("-------------");
    terminal_print_line("Escribe 'help' para ver comandos.");
    terminal_print_line("");
}

static int command_name_matches(const char* input, const char* name) {
    int i = 0;

    while (name[i] != '\0') {
        if (input[i] != name[i]) {
            return 0;
        }
        i++;
    }

    return input[i] == '\0';
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
        terminal_print("[");
        terminal_print_uint((unsigned int)snapshots[i].id);
        terminal_print("] ");
        terminal_print(snapshots[i].name);
        terminal_print(" ");
        terminal_print(task_state_name(snapshots[i].state));
        terminal_print(snapshots[i].foreground ? " FG" : " BG");
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

static int cmd_ls(int argc, const char* argv[], int background) {
    int count;

    (void)argc;
    (void)argv;
    (void)background;

    count = syscall_fs_count();
    if (count <= 0) {
        terminal_print_line("FS vacio");
        return 1;
    }

    for (int i = 0; i < count; i++) {
        const char* name = syscall_fs_name_at(i);
        terminal_print(name);
        terminal_print(" ");
        terminal_print_uint(syscall_fs_size(name));
        terminal_print_line(" bytes");
    }

    return 1;
}

static int cmd_cat(int argc, const char* argv[], int background) {
    char* buffer;
    unsigned int size;
    int read_result;

    (void)background;

    if (argc < 2) {
        terminal_print_line("Uso: cat <NOMBRE>");
        return 1;
    }

    size = syscall_fs_size(argv[1]);
    if (size == 0) {
        terminal_print_line("Archivo no encontrado");
        return 1;
    }

    buffer = (char*)syscall_alloc(size + 1);
    if (buffer == 0) {
        terminal_print_line("Sin memoria para leer archivo");
        return 1;
    }

    read_result = syscall_fs_read(argv[1], buffer, size + 1);
    if (read_result < 0) {
        terminal_print_line("No se pudo leer el archivo");
        syscall_free(buffer);
        return 1;
    }

    syscall_write(buffer);
    if (read_result > 0 && buffer[read_result - 1] != '\n') {
        terminal_put_char('\n');
    }
    syscall_free(buffer);
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
    shell_print_banner();
}

int shell_execute_line(const char* line) {
    char tokens[SHELL_MAX_ARGS][SHELL_TOKEN_MAX];
    const char* argv[SHELL_MAX_ARGS];
    int argc;
    int background = 0;

    if (line == 0) {
        return 1;
    }

    argc = parser_parse_line(line, tokens, argv, SHELL_MAX_ARGS, SHELL_TOKEN_MAX);
    if (argc == 0) {
        return 1;
    }

    if (str_equals(argv[argc - 1], "&")) {
        background = 1;
        argc--;
        if (argc == 0) {
            return 1;
        }
    }

    for (int i = 0; i < command_count; i++) {
        if (command_name_matches(argv[0], commands[i].name)) {
            return commands[i].fn(argc, argv, background);
        }
    }

    terminal_print("Comando no reconocido: ");
    terminal_print_line(argv[0]);
    return 1;
}

int shell_complete_command(const char* prefix, const char* matches[], int max_matches) {
    int found = 0;

    if (prefix == 0) {
        return 0;
    }

    for (int i = 0; i < command_count; i++) {
        int j = 0;

        while (prefix[j] != '\0' && commands[i].name[j] != '\0' && prefix[j] == commands[i].name[j]) {
            j++;
        }

        if (prefix[j] == '\0') {
            if (found < max_matches) {
                matches[found] = commands[i].name;
            }
            found++;
        }
    }

    return found;
}