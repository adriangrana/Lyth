#include "shell_input.h"
#include "shell.h"
#include "terminal.h"
#include "input.h"
#include "task.h"
#include "fs.h"
#include "string.h"

#define PROMPT_LABEL "lyth"
#define PROMPT_SUFFIX "$ "

#define INPUT_MAX 256
#define HISTORY_MAX 16
#define SHELL_INPUT_MAX_VC 4

typedef struct {
    int initialized;
    int prompt_row;
    int prompt_col;
    int rendered_length;
    int prompt_visible;
    char input_buffer[INPUT_MAX];
    int input_length;
    int cursor_pos;
    int selection_active;
    int selection_anchor;
    int history_index;
    int browsing_history;
    char draft_buffer[INPUT_MAX];
} shell_input_vc_state_t;

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
static int overwrite_mode = 0;

static char history[HISTORY_MAX][INPUT_MAX];
static int history_count = 0;
static int history_index = -1;
static char draft_buffer[INPUT_MAX];
static int browsing_history = 0;
static unsigned char prompt_bracket_color = 0x08;
static unsigned char prompt_label_color = 0x0B;
static unsigned char prompt_suffix_color = 0x0A;
static unsigned char selection_color = 0x70;

/* ---- Tab completion suggestion bar ---- */
#define TAB_MAX_MATCHES 8
#define TAB_MATCH_LEN   64

static char tab_match_store[TAB_MAX_MATCHES][TAB_MATCH_LEN];
static int  tab_match_count = 0;
static int  tab_active = 0;
static int  tab_suggest_row_stored = -1;
static shell_input_vc_state_t shell_input_vc_states[SHELL_INPUT_MAX_VC];

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
static void shell_input_move_cursor_home(int extend_selection);
static void shell_input_move_cursor_end(int extend_selection);
static void shell_input_clear_screen_keep_line(void);
static void shell_input_kill_line(void);
static void shell_input_select_all(void);
static void shell_input_try_tab_completion(void);
static void shell_input_clear_suggestions(void);
static void shell_input_refresh_suggestions(void);
static void shell_input_get_current_prefix(char* out, int* first_token_out);

static int shell_input_current_vc_slot(void) {
    int slot = terminal_active_vc();

    if (slot < 0 || slot >= SHELL_INPUT_MAX_VC) {
        return 0;
    }

    return slot;
}

static void shell_input_store_current_vc_state(void) {
    int slot = shell_input_current_vc_slot();

    shell_input_vc_states[slot].initialized = 1;
    shell_input_vc_states[slot].prompt_row = prompt_row;
    shell_input_vc_states[slot].prompt_col = prompt_col;
    shell_input_vc_states[slot].rendered_length = rendered_length;
    shell_input_vc_states[slot].prompt_visible = prompt_visible;
    shell_input_vc_states[slot].input_length = input_length;
    shell_input_vc_states[slot].cursor_pos = cursor_pos;
    shell_input_vc_states[slot].selection_active = selection_active;
    shell_input_vc_states[slot].selection_anchor = selection_anchor;
    shell_input_vc_states[slot].history_index = history_index;
    shell_input_vc_states[slot].browsing_history = browsing_history;
    for (int i = 0; i < input_length; i++)
        shell_input_vc_states[slot].input_buffer[i] = input_buffer[i];
    shell_input_vc_states[slot].input_buffer[input_length] = '\0';
    for (int i = 0; draft_buffer[i] != '\0' && i < INPUT_MAX - 1; i++)
        shell_input_vc_states[slot].draft_buffer[i] = draft_buffer[i];
    shell_input_vc_states[slot].draft_buffer[INPUT_MAX - 1] = '\0';
}

static int shell_input_load_vc_state(int slot) {
    if (slot < 0 || slot >= SHELL_INPUT_MAX_VC || !shell_input_vc_states[slot].initialized) {
        return 0;
    }

    prompt_row = shell_input_vc_states[slot].prompt_row;
    prompt_col = shell_input_vc_states[slot].prompt_col;
    rendered_length = shell_input_vc_states[slot].rendered_length;
    prompt_visible = shell_input_vc_states[slot].prompt_visible;
    input_length = shell_input_vc_states[slot].input_length;
    cursor_pos = shell_input_vc_states[slot].cursor_pos;
    selection_active = shell_input_vc_states[slot].selection_active;
    selection_anchor = shell_input_vc_states[slot].selection_anchor;
    history_index = shell_input_vc_states[slot].history_index;
    browsing_history = shell_input_vc_states[slot].browsing_history;
    for (int i = 0; i < shell_input_vc_states[slot].input_length; i++)
        input_buffer[i] = shell_input_vc_states[slot].input_buffer[i];
    input_buffer[shell_input_vc_states[slot].input_length] = '\0';
    for (int i = 0; i < INPUT_MAX; i++)
        draft_buffer[i] = shell_input_vc_states[slot].draft_buffer[i];
    return 1;
}

static int min_int(int a, int b) {
    return a < b ? a : b;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int shell_input_prompt_length(void) {
    /* Format: [user@lyth:<cwd>]$ */
    return 1 + (int)str_length(shell_get_current_user()) + 1
           + (int)str_length(PROMPT_LABEL) + 1 + (int)str_length(shell_get_cwd())
           + 1 + (int)str_length(PROMPT_SUFFIX);
}

static int shell_input_render_prompt(void) {
    const char* user   = shell_get_current_user();
    const char* label  = PROMPT_LABEL;
    const char* cwd    = shell_get_cwd();
    const char* suffix = PROMPT_SUFFIX;
    int length = 0;

    terminal_put_char_with_color('[', prompt_bracket_color);
    length++;

    for (int i = 0; user[i] != '\0'; i++) {
        terminal_put_char_with_color(user[i], prompt_label_color);
        length++;
    }

    terminal_put_char_with_color('@', prompt_bracket_color);
    length++;

    for (int i = 0; label[i] != '\0'; i++) {
        terminal_put_char_with_color(label[i], prompt_label_color);
        length++;
    }

    terminal_put_char_with_color(':', prompt_bracket_color);
    length++;

    for (int i = 0; cwd[i] != '\0'; i++) {
        terminal_put_char_with_color(cwd[i], 0x0F);
        length++;
    }

    terminal_put_char_with_color(']', prompt_bracket_color);
    length++;

    for (int i = 0; suffix[i] != '\0'; i++) {
        terminal_put_char_with_color(suffix[i], prompt_suffix_color);
        length++;
    }

    return length;
}

static void shell_input_move_cursor_home(int extend_selection) {
    if (extend_selection && !selection_active) {
        selection_active = 1;
        selection_anchor = cursor_pos;
    }

    cursor_pos = 0;

    if (!extend_selection) {
        shell_input_reset_selection();
    }

    shell_input_update_view();
}

static void shell_input_move_cursor_end(int extend_selection) {
    if (extend_selection && !selection_active) {
        selection_active = 1;
        selection_anchor = cursor_pos;
    }

    cursor_pos = input_length;

    if (!extend_selection) {
        shell_input_reset_selection();
    }

    shell_input_update_view();
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
    shell_input_store_current_vc_state();
}

static void shell_input_set_cursor_to_edit_pos(void) {
    int row = 0;
    int col = 0;

    compute_cursor_position(shell_input_prompt_length() + cursor_pos, &row, &col);
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
    int prompt_length;

    terminal_set_cursor(prompt_row, prompt_col);
    prompt_length = shell_input_render_prompt();

    selection_start = -1;
    selection_end = -1;

    if (selection_active && selection_anchor != cursor_pos) {
        selection_start = min_int(selection_anchor, cursor_pos);
        selection_end = max_int(selection_anchor, cursor_pos);
    }

    for (int i = 0; i < input_length; i++) {
        if (i >= selection_start && i < selection_end) {
            terminal_put_char_with_color(input_buffer[i], selection_color);
        } else {
            terminal_put_char(input_buffer[i]);
        }
    }

    while (rendered_length > input_length) {
        terminal_put_char(' ');
        rendered_length--;
    }

    rendered_length = input_length;
    shell_input_store_current_vc_state();
    compute_cursor_position(prompt_length + cursor_pos, &row, &col);
    terminal_set_cursor(row, col);
}

static void shell_input_update_view(void) {
    shell_input_draw_line();
    if (tab_active) {
        shell_input_refresh_suggestions();
    }
}

static void shell_input_show_prompt(void) {
    tab_active = 0;
    tab_match_count = 0;
    tab_suggest_row_stored = -1;
    shell_input_capture_prompt();
    shell_input_render_prompt();
    rendered_length = 0;
    prompt_visible = 1;
    shell_input_store_current_vc_state();
}

static void shell_input_on_foreground_complete(int id, const char* name, int cancelled) {
    (void)id;
    (void)name;
    (void)cancelled;
    prompt_visible = 0;
    shell_input_show_prompt();
    shell_input_update_view();
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
    if (selection_active && selection_anchor != cursor_pos) {
        shell_input_delete_selection();
    }

    if (overwrite_mode && cursor_pos < input_length) {
        input_buffer[cursor_pos] = c;
        cursor_pos++;
        shell_input_reset_selection();
        shell_input_update_view();
        return;
    }

    if (input_length >= INPUT_MAX - 1) {
        return;
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

/* ---- Tab completion helpers ---- */

/* Extract the token at cursor_pos into out; set *first_token_out if it is the first token. */
static void shell_input_get_current_prefix(char* out, int* first_token_out) {
    int ts = cursor_pos;
    int pl = 0;
    int ft = 1;
    int i;

    while (ts > 0 && input_buffer[ts - 1] != ' ' && input_buffer[ts - 1] != '\t')
        ts--;

    for (i = 0; i < ts; i++) {
        if (input_buffer[i] != ' ' && input_buffer[i] != '\t') { ft = 0; break; }
    }

    while ((ts + pl) < cursor_pos && pl < INPUT_MAX - 1) {
        out[pl] = input_buffer[ts + pl];
        pl++;
    }
    out[pl] = '\0';
    *first_token_out = ft;
}

/* Erase the stored suggestion row (if any) and restore cursor to edit pos. */
static void shell_input_erase_suggest_row(void) {
    int cols, i, end_row, end_col, pl;

    if (tab_suggest_row_stored < 0) return;
    if (tab_suggest_row_stored >= terminal_rows()) { tab_suggest_row_stored = -1; return; }

    cols = terminal_columns();
    terminal_set_cursor(tab_suggest_row_stored, 0);
    for (i = 0; i < cols; i++) terminal_put_char_with_color(' ', 0x00);
    tab_suggest_row_stored = -1;

    pl = shell_input_prompt_length();
    compute_cursor_position(pl + cursor_pos, &end_row, &end_col);
    terminal_set_cursor(end_row, end_col);
}

static void shell_input_clear_suggestions(void) {
    if (!tab_active) return;
    shell_input_erase_suggest_row();
    tab_active = 0;
    tab_match_count = 0;
}

static void shell_input_refresh_suggestions(void) {
    char prefix[INPUT_MAX];
    int  first_token;
    const char* filtered[TAB_MAX_MATCHES];
    int  filtered_count = 0;
    int  new_row, cols, col_pos, pl, end_row, end_col;
    int  i;

    if (!tab_active) return;

    shell_input_get_current_prefix(prefix, &first_token);

    /* Filter stored matches by current prefix */
    for (i = 0; i < tab_match_count; i++) {
        if (str_starts_with_ignore_case(tab_match_store[i], prefix))
            filtered[filtered_count++] = tab_match_store[i];
    }

    if (filtered_count == 0) {
        shell_input_clear_suggestions();
        return;
    }

    /* Compute where to draw (row after the input) */
    pl = shell_input_prompt_length();
    compute_cursor_position(pl + input_length, &end_row, &end_col);
    new_row = (end_col == 0) ? end_row : end_row + 1;

    /* Erase old row if it moved */
    if (tab_suggest_row_stored >= 0 && tab_suggest_row_stored != new_row) {
        int old = tab_suggest_row_stored;
        tab_suggest_row_stored = -1;
        cols = terminal_columns();
        terminal_set_cursor(old, 0);
        for (i = 0; i < cols; i++) terminal_put_char_with_color(' ', 0x00);
    }

    if (new_row >= terminal_rows()) {
        compute_cursor_position(pl + cursor_pos, &end_row, &end_col);
        terminal_set_cursor(end_row, end_col);
        return;
    }

    tab_suggest_row_stored = new_row;
    cols = terminal_columns();
    terminal_set_cursor(new_row, 0);
    col_pos = 0;

    for (i = 0; i < filtered_count; i++) {
        const char* m = filtered[i];
        int j;
        if (i > 0 && col_pos + 2 < cols) {
            terminal_put_char_with_color(' ', 0x08);
            terminal_put_char_with_color(' ', 0x08);
            col_pos += 2;
        }
        for (j = 0; m[j] && col_pos < cols - 1; j++, col_pos++)
            terminal_put_char_with_color(m[j], 0x0E);
    }
    /* Clear remainder of the line */
    while (col_pos < cols) {
        terminal_put_char_with_color(' ', 0x00);
        col_pos++;
    }

    /* Restore cursor to edit position */
    compute_cursor_position(pl + cursor_pos, &end_row, &end_col);
    terminal_set_cursor(end_row, end_col);
}

static void shell_input_try_tab_completion(void) {
    const char* matches[TAB_MAX_MATCHES];
    char prefix[INPUT_MAX];
    int  token_start, token_end, prefix_length = 0;
    int  match_count, first_token = 1, replace_end;
    int  i;

    /* Always clear any active suggestion bar first */
    shell_input_clear_suggestions();

    if (cursor_pos > input_length) cursor_pos = input_length;

    token_start = cursor_pos;
    while (token_start > 0 && input_buffer[token_start - 1] != ' ' && input_buffer[token_start - 1] != '\t')
        token_start--;

    token_end = cursor_pos;
    while (token_end < input_length && input_buffer[token_end] != ' ' && input_buffer[token_end] != '\t')
        token_end++;

    for (i = 0; i < token_start; i++) {
        if (input_buffer[i] != ' ' && input_buffer[i] != '\t') { first_token = 0; break; }
    }

    while ((token_start + prefix_length) < cursor_pos && prefix_length < INPUT_MAX - 1) {
        prefix[prefix_length] = input_buffer[token_start + prefix_length];
        prefix_length++;
    }
    prefix[prefix_length] = '\0';

    if (prefix_length == 0 && first_token) return;

    match_count = first_token ? shell_complete_command(prefix, matches, TAB_MAX_MATCHES)
                              : shell_complete_path(prefix, matches, TAB_MAX_MATCHES);
    if (match_count == 0) return;

    if (match_count == 1) {
        /* Auto-complete the single match */
        int write_index = token_start;
        const char* completion = matches[0];
        while (completion[0] != '\0' && write_index < INPUT_MAX - 1) {
            input_buffer[write_index++] = *completion++;
        }
        replace_end = token_end;
        while (replace_end < input_length) {
            input_buffer[write_index++] = input_buffer[replace_end++];
        }
        if (token_end == input_length && write_index < INPUT_MAX - 1)
            input_buffer[write_index++] = ' ';
        input_buffer[write_index] = '\0';
        input_length = write_index;
        cursor_pos = input_length;
        shell_input_reset_selection();
        shell_input_update_view();
        return;
    }

    /* Multiple matches: store and show inline suggestion bar */
    tab_active = 1;
    tab_match_count = (match_count > TAB_MAX_MATCHES) ? TAB_MAX_MATCHES : match_count;
    tab_suggest_row_stored = -1;
    for (i = 0; i < tab_match_count; i++) {
        int j;
        for (j = 0; matches[i][j] && j < TAB_MATCH_LEN - 1; j++)
            tab_match_store[i][j] = matches[i][j];
        tab_match_store[i][j] = '\0';
    }
    shell_input_refresh_suggestions();
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

    shell_input_clear_suggestions();
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
    for (int i = 0; i < SHELL_INPUT_MAX_VC; i++) {
        shell_input_vc_states[i].initialized = 0;
        shell_input_vc_states[i].prompt_row = 0;
        shell_input_vc_states[i].prompt_col = 0;
        shell_input_vc_states[i].rendered_length = 0;
        shell_input_vc_states[i].prompt_visible = 0;
        shell_input_vc_states[i].input_length = 0;
        shell_input_vc_states[i].cursor_pos = 0;
        shell_input_vc_states[i].selection_active = 0;
        shell_input_vc_states[i].selection_anchor = 0;
        shell_input_vc_states[i].history_index = -1;
        shell_input_vc_states[i].browsing_history = 0;
        shell_input_vc_states[i].input_buffer[0] = '\0';
        shell_input_vc_states[i].draft_buffer[0] = '\0';
    }

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

void shell_input_set_theme(unsigned char bracket_color, unsigned char label_color, unsigned char suffix_color, unsigned char new_selection_color) {
    prompt_bracket_color = bracket_color;
    prompt_label_color = label_color;
    prompt_suffix_color = suffix_color;
    selection_color = new_selection_color;
}

static void shell_input_toggle_overwrite_mode(void) {
    overwrite_mode = !overwrite_mode;
    terminal_set_overwrite_mode(overwrite_mode);
    shell_input_update_view();
}

static int shell_input_try_switch_virtual_console(const input_event_t* event) {
    int target_vc = -1;
    int current_vc;
    int had_state;

    if (event == 0 || event->device_type != INPUT_DEVICE_KEYBOARD || (event->modifiers & KEY_MOD_CTRL) == 0) {
        return 0;
    }

    switch (event->type) {
        case INPUT_EVENT_F1:
            target_vc = 0;
            break;
        case INPUT_EVENT_F2:
            target_vc = 1;
            break;
        case INPUT_EVENT_F3:
            target_vc = 2;
            break;
        case INPUT_EVENT_F4:
            target_vc = 3;
            break;
        default:
            return 0;
    }

    current_vc = shell_input_current_vc_slot();
    shell_input_clear_suggestions();
    shell_input_store_current_vc_state();

    if (!terminal_switch_vc(target_vc)) {
        return 0;
    }

    had_state = shell_input_load_vc_state(target_vc);
    if (!had_state) {
        /* First visit to this VC — draw a fresh prompt */
        shell_input_show_prompt();
        shell_input_update_view();
    } else if (prompt_visible) {
        shell_input_update_view();
    }

    (void)current_vc;
    return 1;
}

void shell_input_handle_event(const input_event_t* event) {
    if (event == 0) {
        return;
    }

    if (event->device_type != INPUT_DEVICE_KEYBOARD) {
        return;
    }

    if (shell_input_try_switch_virtual_console(event)) {
        return;
    }

    if (task_has_foreground_task()) {
        if (event->type == INPUT_EVENT_CTRL_C) {
            task_request_foreground_cancel();
        }
        return;
    }

    if (!prompt_visible) {
        return;
    }

    switch (event->type) {
        case INPUT_EVENT_CTRL_C:
            shell_input_clear_suggestions();
            terminal_print_line("^C");
            input_length = 0;
            cursor_pos = 0;
            input_buffer[0] = '\0';
            shell_input_reset_selection();
            history_end_browse();
            shell_input_show_prompt();
            shell_input_update_view();
            return;

        case INPUT_EVENT_ENTER:
            shell_input_submit();
            return;

        case INPUT_EVENT_BACKSPACE:
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

        case INPUT_EVENT_DELETE:
            if (selection_active && selection_anchor != cursor_pos) {
                shell_input_delete_selection();
            } else if (cursor_pos < input_length && input_length > 0) {
                for (int i = cursor_pos; i < input_length - 1; i++) {
                    input_buffer[i] = input_buffer[i + 1];
                }
                input_length--;
                input_buffer[input_length] = '\0';
                shell_input_reset_selection();
                shell_input_update_view();
            }
            history_end_browse();
            return;

        case INPUT_EVENT_UP:
            history_move_up();
            return;

        case INPUT_EVENT_DOWN:
            history_move_down();
            return;

        case INPUT_EVENT_LEFT:
            shell_input_move_cursor_left((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case INPUT_EVENT_RIGHT:
            shell_input_move_cursor_right((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case INPUT_EVENT_HOME:
            shell_input_move_cursor_home((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case INPUT_EVENT_END:
            shell_input_move_cursor_end((event->modifiers & KEY_MOD_SHIFT) != 0);
            history_end_browse();
            return;

        case INPUT_EVENT_PAGE_UP:
            history_move_up();
            return;

        case INPUT_EVENT_PAGE_DOWN:
            history_move_down();
            return;

        case INPUT_EVENT_INSERT:
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

            shell_input_toggle_overwrite_mode();
            return;

        case INPUT_EVENT_TAB:
            shell_input_try_tab_completion();
            return;

        case INPUT_EVENT_CHAR:
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
