#include "shell.h"
#include "terminal.h"
#include "string.h"

#define INPUT_MAX 256
#define HISTORY_MAX 8

typedef void (*command_fn)(const char* args);

typedef struct {
    const char* name;
    command_fn fn;
    const char* help;
} command_t;

static char input_buffer[INPUT_MAX];
static int input_pos = 0;

static char history[HISTORY_MAX][INPUT_MAX];
static int history_count = 0;

static void shell_show_prompt(void) {
    terminal_print("> ");
}

static void history_add(const char* line) {
    if (line[0] == '\0') {
        return;
    }

    if (history_count < HISTORY_MAX) {
        int i = 0;
        while (line[i] != '\0' && i < INPUT_MAX - 1) {
            history[history_count][i] = line[i];
            i++;
        }
        history[history_count][i] = '\0';
        history_count++;
        return;
    }

    for (int h = 1; h < HISTORY_MAX; h++) {
        int i = 0;
        while (history[h][i] != '\0' && i < INPUT_MAX - 1) {
            history[h - 1][i] = history[h][i];
            i++;
        }
        history[h - 1][i] = '\0';
    }

    int i = 0;
    while (line[i] != '\0' && i < INPUT_MAX - 1) {
        history[HISTORY_MAX - 1][i] = line[i];
        i++;
    }
    history[HISTORY_MAX - 1][i] = '\0';
}

static const char* get_args(const char* cmd) {
    int i = 0;

    while (cmd[i] != '\0' && cmd[i] != ' ') {
        i++;
    }

    if (cmd[i] == ' ') {
        i++;
    }

    return cmd + i;
}

static int command_name_matches(const char* input, const char* name) {
    int i = 0;

    while (name[i] != '\0') {
        if (input[i] != name[i]) {
            return 0;
        }
        i++;
    }

    return input[i] == '\0' || input[i] == ' ';
}

/* ===== comandos ===== */

static void cmd_help(const char* args);
static void cmd_clear(const char* args);
static void cmd_about(const char* args);
static void cmd_echo(const char* args);
static void cmd_color(const char* args);
static void cmd_history(const char* args);

static command_t commands[] = {
    {"help",    cmd_help,    "muestra esta ayuda"},
    {"clear",   cmd_clear,   "limpia la pantalla"},
    {"cls",     cmd_clear,   "alias de clear"},
    {"about",   cmd_about,   "informacion del sistema"},
    {"echo",    cmd_echo,    "muestra texto"},
    {"color",   cmd_color,   "cambia color: color green|red|white|blue"},
    {"history", cmd_history, "muestra historial"},
};

static const int command_count = sizeof(commands) / sizeof(commands[0]);

static void cmd_help(const char* args) {
    (void)args;
    terminal_print_line("Comandos disponibles:");
    for (int i = 0; i < command_count; i++) {
        terminal_print("  ");
        terminal_print(commands[i].name);
        terminal_print(" - ");
        terminal_print_line(commands[i].help);
    }
}

static void cmd_clear(const char* args) {
    (void)args;
    terminal_clear();
}

static void cmd_about(const char* args) {
    (void)args;
    terminal_print_line("Mi OS v0.2");
    terminal_print_line("Kernel hobby en C + ASM");
    terminal_print_line("Shell modular funcionando");
}

static void cmd_echo(const char* args) {
    terminal_print_line(args);
}

static void cmd_color(const char* args) {
    if (str_equals(args, "white")) {
        terminal_set_color(0x0F);
        terminal_print_line("Color cambiado a white");
        return;
    }

    if (str_equals(args, "green")) {
        terminal_set_color(0x0A);
        terminal_print_line("Color cambiado a green");
        return;
    }

    if (str_equals(args, "red")) {
        terminal_set_color(0x0C);
        terminal_print_line("Color cambiado a red");
        return;
    }

    if (str_equals(args, "blue")) {
        terminal_set_color(0x09);
        terminal_print_line("Color cambiado a blue");
        return;
    }

    terminal_print_line("Uso: color green|red|white|blue");
}

static void cmd_history(const char* args) {
    (void)args;

    if (history_count == 0) {
        terminal_print_line("Historial vacio");
        return;
    }

    for (int i = 0; i < history_count; i++) {
        terminal_print("- ");
        terminal_print_line(history[i]);
    }
}

static void shell_execute(const char* line) {
    if (line[0] == '\0') {
        return;
    }

    for (int i = 0; i < command_count; i++) {
        if (command_name_matches(line, commands[i].name)) {
            commands[i].fn(get_args(line));
            return;
        }
    }

    terminal_print("Comando no reconocido: ");
    terminal_print_line(line);
}

/* ===== interfaz pública ===== */

void shell_init(void) {
    input_pos = 0;
    input_buffer[0] = '\0';

    terminal_print_line("Mini shell de Mi OS");
    terminal_print_line("-------------------");
    terminal_print_line("Escribe 'help' para ver comandos.");
    terminal_print_line("");

    shell_show_prompt();
}

void shell_handle_char(char c) {
    if (c == '\t') {
        return;
    }

    if (c == '\b') {
        if (input_pos > 0) {
            input_pos--;
            input_buffer[input_pos] = '\0';
            terminal_backspace();
        }
        return;
    }

    if (c == '\n') {
        terminal_put_char('\n');
        input_buffer[input_pos] = '\0';

        history_add(input_buffer);
        shell_execute(input_buffer);

        input_pos = 0;
        input_buffer[0] = '\0';
        shell_show_prompt();
        return;
    }

    if (input_pos < INPUT_MAX - 1) {
        input_buffer[input_pos] = c;
        input_pos++;
        input_buffer[input_pos] = '\0';
        terminal_put_char(c);
    }
}