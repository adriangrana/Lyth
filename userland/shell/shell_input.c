#include "shell_input.h"
#include "shell.h"
#include "terminal.h"
#include "keyboard.h"
#include "task.h"

#define PROMPT_TEXT "> "

#define INPUT_MAX 256
#define HISTORY_MAX 16

static char input_buffer[INPUT_MAX];
static int input_length = 0;
static int cursor_pos = 0;
static int prompt_row = 0;
static int prompt_col = 0;
static int rendered_length = 0;
static int prompt_visible = 0;

static int selection_active = 0;
static int selection_anchor = 0;

static char clipboard[INPUT_MAX];
static int clipboard_length = 0;

static char history[HISTORY_MAX][INPUT_MAX];
static int history_count = 0;
static int history_index = -1;
static char draft_buffer[INPUT_MAX];
static int browsing_history = 0;

static void shell_input_update_view(void);
static void shell_input_reset_selection(void);
static void shell_input_start_selection(void);
static void shell_input_copy_selection(void);
static void shell_input_copy_all(void);
static void shell_input_delete_selection(void);
static void shell_input_cut_selection_or_line(void);
static void shell_input_insert_character(char c);
static void shell_input_paste_clipboard(void);
static void shell_input_move_cursor_left(int extend_selection);
static void shell_input_move_cursor_right(int extend_selection);
static void shell_input_clear_screen_keep_line(void);
static void shell_input_kill_line(void);
static void shell_input_select_all(void);
static void shell_input_try_tab_completion(void);

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void compute_cursor_position(int offset, int* row, int* col) {
    int width = terminal_columns();
    int absolute;

    if (width <= 0) {
        width = 80;
    }

    absolute = prompt_row * width + prompt_col + offset;
    *row = absolute / width;
    *col = absolute % width;
}

static void shell_input_capture_prompt(void) {
    terminal_get_cursor(&prompt_row, &prompt_col);
}

static void shell_input_set_cursor_to_edit_pos(void) {
    int row = 0;
    int col = 0;

    compute_cursor_position(2 + cursor_pos, &row, &col);
    terminal_set_cursor(row, col);
}

static void shell_input_reset_selection(void) {
    selection_active = 0;
    selection_anchor = cursor_pos;
}

static void shell_input_start_selection(void) {
    if (!selection_active) {
        selection_active = 1;
        selection_anchor = cursor_pos;
    }
}

static void shell_input_copy_selection(void) {
    int start;
    int end;

    if (!selection_active || selection_anchor == cursor_pos) {
        return;
    }

    start = min_int(selection_anchor, cursor_pos);
    end = max_int(selection_anchor, cursor_pos);
    clipboard_length = 0;

    while (start < end && clipboard_length < INPUT_MAX - 1) {
        clipboard[clipboard_length] = input_buffer[start];
        clipboard_length++;
        start++;
    }

    clipboard[clipboard_length] = '\0';
}

static void shell_input_copy_all(void) {
    int i = 0;

    clipboard_length = 0;

    while (i < input_length && clipboard_length < INPUT_MAX - 1) {
        clipboard[clipboard_length] = input_buffer[i];
        clipboard_length++;
        i++;
    }

    clipboard[clipboard_length] = '\0';
}

static void shell_input_delete_selection(void) {
    int start;
    int end;
    int write_index;
    int read_index;

    if (!selection_active || selection_anchor == cursor_pos) {
        return;
    }

    start = min_int(selection_anchor, cursor_pos);
    end = max_int(selection_anchor, cursor_pos);

    write_index = start;
    read_index = end;

    while (read_index < input_length) {
        input_buffer[write_index] = input_buffer[read_index];
        write_index++;
        read_index++;
    }

    input_length -= (end - start);
    input_buffer[input_length] = '\0';
    cursor_pos = start;
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_cut_selection_or_line(void) {
    if (selection_active && selection_anchor != cursor_pos) {
        shell_input_copy_selection();
        shell_input_delete_selection();
        return;
    }

    if (input_length == 0) {
        return;
    }

    shell_input_copy_all();
    input_length = 0;
    cursor_pos = 0;
    input_buffer[0] = '\0';
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_insert_text(const char* text) {
    int text_index = 0;

    if (selection_active && selection_anchor != cursor_pos) {
        shell_input_delete_selection();
    }

    while (text[text_index] != '\0' && input_length < INPUT_MAX - 1) {
        int tail_index = input_length;

        while (tail_index > cursor_pos) {
            input_buffer[tail_index] = input_buffer[tail_index - 1];
            tail_index--;
        }

        input_buffer[cursor_pos] = text[text_index];
        cursor_pos++;
        input_length++;
        text_index++;
    }

    input_buffer[input_length] = '\0';
}

static void shell_input_draw_line(void) {
    int row;
    int col;
    int selection_start;
    int selection_end;

    terminal_set_cursor(prompt_row, prompt_col);
    terminal_print(PROMPT_TEXT);

    selection_start = -1;
    selection_end = -1;

    if (selection_active && selection_anchor != cursor_pos) {
        selection_start = min_int(selection_anchor, cursor_pos);
        selection_end = max_int(selection_anchor, cursor_pos);
    }

    for (int i = 0; i < input_length; i++) {
        if (i >= selection_start && i < selection_end) {
            terminal_put_char_with_color(input_buffer[i], 0x70);
        } else {
            terminal_put_char(input_buffer[i]);
        }
    }

    while (rendered_length > input_length) {
        terminal_put_char(' ');
        rendered_length--;
    }

    rendered_length = input_length;
    compute_cursor_position(2 + cursor_pos, &row, &col);
    terminal_set_cursor(row, col);
}

static void shell_input_update_view(void) {
    shell_input_draw_line();
}

static void shell_input_show_prompt(void) {
    shell_input_capture_prompt();
    terminal_print(PROMPT_TEXT);
    rendered_length = 0;
    prompt_visible = 1;
}

static void shell_input_on_foreground_complete(int id, const char* name, int cancelled) {
    (void)id;
    (void)name;
    (void)cancelled;
    shell_input_resume_prompt();
}

static void shell_input_clear_line(void) {
    input_length = 0;
    cursor_pos = 0;
    input_buffer[0] = '\0';
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_replace_line(const char* text) {
    int i = 0;

    while (text[i] != '\0' && i < INPUT_MAX - 1) {
        input_buffer[i] = text[i];
        i++;
    }

    input_buffer[i] = '\0';
    input_length = i;
    cursor_pos = input_length;
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_insert_character(char c) {
    if (input_length >= INPUT_MAX - 1) {
        return;
    }

    if (selection_active && selection_anchor != cursor_pos) {
        shell_input_delete_selection();
    }

    for (int i = input_length; i > cursor_pos; i--) {
        input_buffer[i] = input_buffer[i - 1];
    }

    input_buffer[cursor_pos] = c;
    cursor_pos++;
    input_length++;
    input_buffer[input_length] = '\0';
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_paste_clipboard(void) {
    for (int i = 0; i < clipboard_length; i++) {
        shell_input_insert_character(clipboard[i]);
    }
}

static void shell_input_move_cursor_left(int extend_selection) {
    if (cursor_pos <= 0) {
        return;
    }

    if (extend_selection && !selection_active) {
        selection_active = 1;
        selection_anchor = cursor_pos;
    }

    cursor_pos--;

    if (!extend_selection) {
        shell_input_reset_selection();
    }

    shell_input_update_view();
}

static void shell_input_move_cursor_right(int extend_selection) {
    if (cursor_pos >= input_length) {
        return;
    }

    if (extend_selection && !selection_active) {
        selection_active = 1;
        selection_anchor = cursor_pos;
    }

    cursor_pos++;

    if (!extend_selection) {
        shell_input_reset_selection();
    }

    shell_input_update_view();
}

static void shell_input_clear_screen_keep_line(void) {
    terminal_clear();
    shell_input_show_prompt();
    shell_input_update_view();
}

static void shell_input_kill_line(void) {
    input_length = 0;
    cursor_pos = 0;
    input_buffer[0] = '\0';
    shell_input_reset_selection();
    shell_input_update_view();
}

static void shell_input_select_all(void) {
    if (input_length == 0) {
        return;
    }

    selection_active = 1;
    selection_anchor = 0;
    cursor_pos = input_length;
    shell_input_update_view();
}

static void shell_input_try_tab_completion(void) {
    const char* matches[8];
    char prefix[INPUT_MAX];
    int prefix_length = 0;
    int match_count;

    for (int i = 0; i < input_length; i++) {
        if (input_buffer[i] == ' ' || input_buffer[i] == '\t') {
            return;
        }
    }

    while (prefix_length < cursor_pos && prefix_length < INPUT_MAX - 1 && input_buffer[prefix_length] != ' ' && input_buffer[prefix_length] != '\t') {
        prefix[prefix_length] = input_buffer[prefix_length];
        prefix_length++;
    }

    prefix[prefix_length] = '\0';

    if (prefix_length == 0) {
        return;
    }

    match_count = shell_complete_command(prefix, matches, 8);
    if (match_count == 0) {
        return;
    }

    if (match_count == 1) {
        int match_index = 0;
        const char* completion = matches[0];

        while (completion[match_index] != '\0' && match_index < INPUT_MAX - 1) {
            input_buffer[match_index] = completion[match_index];
            match_index++;
        }

        if (match_index < INPUT_MAX - 1) {
            input_buffer[match_index] = ' ';
            match_index++;
        }

        input_buffer[match_index] = '\0';
        input_length = match_index;
        cursor_pos = input_length;
        shell_input_reset_selection();
        shell_input_update_view();
        return;
    }

    terminal_put_char('\n');
    for (int i = 0; i < match_count && i < 8; i++) {
        terminal_print(matches[i]);
        terminal_print("  ");
    }
    terminal_put_char('\n');
    shell_input_update_view();
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

static void history_begin_browse(void) {
    if (!browsing_history) {
        int i = 0;
        while (input_buffer[i] != '\0' && i < INPUT_MAX - 1) {
            draft_buffer[i] = input_buffer[i];
            i++;
        }
        draft_buffer[i] = '\0';
        browsing_history = 1;
    }
}

static void history_end_browse(void) {
    browsing_history = 0;
    history_index = -1;
}

static void history_show_current(void) {
    if (history_index >= 0 && history_index < history_count) {
        shell_input_replace_line(history[history_index]);
    }
}

static void history_move_up(void) {
    if (history_count == 0) {
        return;
    }

    history_begin_browse();

    if (history_index < 0) {
        history_index = history_count - 1;
    } else if (history_index > 0) {
        history_index--;
    }

    history_show_current();
}

static void history_move_down(void) {
    if (!browsing_history) {
        return;
    }

    if (history_index >= 0 && history_index < history_count - 1) {
        history_index++;
        history_show_current();
        return;
    }

    shell_input_replace_line(draft_buffer);
    history_end_browse();
}

static void shell_input_submit(void) {
    int show_prompt;

    terminal_put_char('\n');
    input_buffer[input_length] = '\0';

    history_add(input_buffer);
    show_prompt = shell_execute_line(input_buffer);

    input_length = 0;
    cursor_pos = 0;
    input_buffer[0] = '\0';
    history_end_browse();

    if (show_prompt) {
        shell_input_show_prompt();
    } else {
        prompt_visible = 0;
    }
}

void shell_input_init(void) {
    input_length = 0;
    cursor_pos = 0;
    input_buffer[0] = '\0';
    history_index = -1;
    browsing_history = 0;
    draft_buffer[0] = '\0';
    rendered_length = 0;
    selection_active = 0;
    selection_anchor = 0;
    clipboard_length = 0;
    prompt_visible = 0;

    shell_init();
    task_set_foreground_complete_handler(shell_input_on_foreground_complete);
    shell_input_show_prompt();
}

void shell_input_resume_prompt(void) {
    if (prompt_visible) {
        return;
    }

    shell_input_show_prompt();
    shell_input_update_view();
}

void shell_input_handle_event(const keyboard_event_t* event) {
    if (event == 0) {
        return;
    }

    if (!prompt_visible) {
        if (event->type == KEY_EVENT_CTRL_C && task_has_foreground_task()) {
            return;
        }

        return;
    }

    switch (event->type) {
        case KEY_EVENT_CTRL_C:
            terminal_print_line("^C");
            input_length = 0;
            cursor_pos = 0;
            input_buffer[0] = '\0';
            shell_input_reset_selection();
            history_end_browse();
            shell_input_show_prompt();
            shell_input_update_view();
            return;

        case KEY_EVENT_ENTER:
            shell_input_submit();
            return;

        case KEY_EVENT_BACKSPACE:
            if (selection_active && selection_anchor != cursor_pos) {
                shell_input_delete_selection();
            } else if (cursor_pos > 0 && input_length > 0) {
                for (int i = cursor_pos - 1; i < input_length - 1; i++) {
                    input_buffer[i] = input_buffer[i + 1];
                }
                cursor_pos--;
                input_length--;
                input_buffer[input_length] = '\0';
                shell_input_reset_selection();
                shell_input_update_view();
            }
            history_end_browse();
            return;

        case KEY_EVENT_UP:
            history_move_up();
            return;

        case KEY_EVENT_DOWN:
            history_move_down();
            return;

        case KEY_EVENT_LEFT:
            shell_input_move_cursor_left((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case KEY_EVENT_RIGHT:
            shell_input_move_cursor_right((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case KEY_EVENT_INSERT:
            if (event->modifiers & KEY_MOD_SHIFT) {
                shell_input_paste_clipboard();
                shell_input_update_view();
                return;
            }

            if (event->modifiers & KEY_MOD_CTRL) {
                if (!selection_active || selection_anchor == cursor_pos) {
                    shell_input_copy_all();
                } else {
                    shell_input_copy_selection();
                }
                return;
            }

            return;

        case KEY_EVENT_TAB:
            shell_input_try_tab_completion();
            return;

        case KEY_EVENT_CHAR:
            if (event->modifiers & KEY_MOD_CTRL) {
                if (event->character == 'l' || event->character == 'L') {
                    shell_input_clear_screen_keep_line();
                    return;
                }

                if (event->character == 'u' || event->character == 'U') {
                    shell_input_kill_line();
                    history_end_browse();
                    return;
                }

                if (event->character == 'a' || event->character == 'A') {
                    shell_input_select_all();
                    return;
                }

                if (event->character == 'v' || event->character == 'V') {
                    shell_input_paste_clipboard();
                    shell_input_update_view();
                    return;
                }

                if (event->character == 'x' || event->character == 'X') {
                    shell_input_cut_selection_or_line();
                    history_end_browse();
                    return;
                }

                if (event->character == 'c' || event->character == 'C') {
                    if (event->modifiers & KEY_MOD_SHIFT) {
                        shell_input_copy_selection();
                        if (!selection_active || selection_anchor == cursor_pos) {
                            shell_input_copy_all();
                        }
                        return;
                    }

                    terminal_print_line("^C");
                    shell_input_kill_line();
                    history_end_browse();
                    shell_input_show_prompt();
                    shell_input_update_view();
                    return;
                }

            }

            if (input_length < INPUT_MAX - 1) {
                shell_input_insert_character(event->character);
            }
            history_end_browse();
            return;

        default:
            return;
    }
}

void shell_input_print_history(void) {
    if (history_count == 0) {
        terminal_print_line("Historial vacio");
        return;
    }

    for (int i = 0; i < history_count; i++) {
        terminal_print("- ");
        terminal_print_line(history[i]);
    }
}
