#include "terminal.h"
#include "console_backend.h"
#include "timer.h"
#include "heap.h"
#include "serial.h"
#include "utf8.h"
#include <stdint.h>

#define TERMINAL_MAX_COLUMNS 240
#define TERMINAL_MAX_ROWS 80
#define TERMINAL_VC_COUNT 4

typedef struct {
    unsigned int glyph;  /* CP437 font glyph index */
    unsigned char color;
} terminal_cell_t;

typedef struct {
    terminal_cell_t cells[TERMINAL_MAX_ROWS][TERMINAL_MAX_COLUMNS];
    unsigned char color;
    int cursor_row;
    int cursor_col;
    uint32_t utf8_accum;
    int utf8_remain;
} terminal_vc_state_t;

static int terminal_overwrite_cursor = 0;
static char* terminal_capture_buffer = 0;
static unsigned int terminal_capture_size = 0;
static unsigned int terminal_capture_length = 0;
static int terminal_capture_active = 0;
static int terminal_capture_dynamic = 0;

static terminal_vc_state_t terminal_vcs[TERMINAL_VC_COUNT];
static int terminal_active_console = 0;
static int terminal_output_vc_override = -1;
static int terminal_cols = 80;
static int terminal_rows_count = 25;

static int cursor_visible = 0;
static uint32_t last_blink = 0;

static int clamp_positive(int value, int fallback) {
    return value > 0 ? value : fallback;
}

static const console_backend_t* terminal_backend(void) {
    return console_backend_current();
}

static terminal_vc_state_t* active_vc(void) {
    return &terminal_vcs[terminal_active_console];
}

static const terminal_vc_state_t* active_vc_const(void) {
    return &terminal_vcs[terminal_active_console];
}

static int output_vc_index(void) {
    if (terminal_output_vc_override >= 0 && terminal_output_vc_override < TERMINAL_VC_COUNT)
        return terminal_output_vc_override;
    return terminal_active_console;
}

static terminal_vc_state_t* output_vc(void) {
    return &terminal_vcs[output_vc_index()];
}

static int output_is_visible(void) {
    return output_vc_index() == terminal_active_console;
}

static void sync_terminal_geometry(void) {
    const console_backend_t* backend = terminal_backend();
    int columns = clamp_positive(backend->columns(), 80);
    int rows = clamp_positive(backend->rows(), 25);

    if (columns > TERMINAL_MAX_COLUMNS) {
        columns = TERMINAL_MAX_COLUMNS;
    }

    if (rows > TERMINAL_MAX_ROWS) {
        rows = TERMINAL_MAX_ROWS;
    }

    terminal_cols = columns;
    terminal_rows_count = rows;
}

static void render_cell_vc(const terminal_vc_state_t* vc, int row, int col) {
    const console_backend_t* backend = terminal_backend();

    if (row < 0 || row >= terminal_rows_count || col < 0 || col >= terminal_cols) {
        return;
    }

    backend->put_cell(
        row,
        col,
        vc->cells[row][col].glyph,
        vc->cells[row][col].color
    );
}

static void render_active_console(void) {
    const console_backend_t* backend = terminal_backend();

    backend->clear(active_vc()->color);
    for (int row = 0; row < terminal_rows_count; row++) {
        for (int col = 0; col < terminal_cols; col++) {
            render_cell_vc(active_vc_const(), row, col);
        }
    }
}

static void terminal_capture_put_char(char c) {
    char* next_buffer;
    unsigned int next_size;
    unsigned int i;

    if (!terminal_capture_active || terminal_capture_buffer == 0 || terminal_capture_size == 0) {
        return;
    }

    if (terminal_capture_length >= terminal_capture_size - 1) {
        if (terminal_capture_dynamic) {
            next_size = terminal_capture_size < 64U ? 64U : terminal_capture_size * 2U;
            next_buffer = (char*)kmalloc(next_size);
            if (next_buffer == 0) {
                terminal_capture_buffer[terminal_capture_size - 1] = '\0';
                return;
            }

            for (i = 0; i < terminal_capture_length; i++) {
                next_buffer[i] = terminal_capture_buffer[i];
            }
            next_buffer[terminal_capture_length] = '\0';
            kfree(terminal_capture_buffer);
            terminal_capture_buffer = next_buffer;
            terminal_capture_size = next_size;
        }

        terminal_capture_buffer[terminal_capture_size - 1] = '\0';
        if (terminal_capture_length >= terminal_capture_size - 1) {
            return;
        }
    }

    terminal_capture_buffer[terminal_capture_length++] = c;
    terminal_capture_buffer[terminal_capture_length] = '\0';
}

static void hide_cursor(void) {
    const console_backend_t* backend = terminal_backend();
    const terminal_vc_state_t* vc = active_vc_const();

    if (!backend->software_cursor || !cursor_visible) {
        return;
    }

    render_cell_vc(vc, vc->cursor_row, vc->cursor_col);
    cursor_visible = 0;
}

static void show_cursor(void) {
    const console_backend_t* backend = terminal_backend();
    const terminal_vc_state_t* vc = active_vc_const();

    backend->show_cursor(vc->cursor_row, vc->cursor_col, vc->color);

    if (backend->software_cursor) {
        cursor_visible = 1;
    }
}

void terminal_update_cursor(void) {
    const console_backend_t* backend = terminal_backend();
    uint32_t now = timer_get_ticks();
    const uint32_t blink_interval = 50; /* PIT a 100 Hz -> 0.5 s */

    if (!backend->software_cursor) {
        show_cursor();
        return;
    }

    if ((now - last_blink) < blink_interval) {
        return;
    }

    last_blink = now;

    if (cursor_visible) {
        hide_cursor();
    } else {
        show_cursor();
    }
}

static void reset_screen_buffer(terminal_vc_state_t* vc) {
    for (int row = 0; row < terminal_rows_count; row++) {
        for (int col = 0; col < terminal_cols; col++) {
            vc->cells[row][col].glyph = ' ';
            vc->cells[row][col].color = vc->color;
        }
    }
}

static void scroll_if_needed(void) {
    const console_backend_t* backend = terminal_backend();
    terminal_vc_state_t* vc = output_vc();
    int visible = output_is_visible();

    if (vc->cursor_row < terminal_rows_count) {
        return;
    }

    for (int row = 1; row < terminal_rows_count; row++) {
        for (int col = 0; col < terminal_cols; col++) {
            vc->cells[row - 1][col] = vc->cells[row][col];
        }
    }

    for (int col = 0; col < terminal_cols; col++) {
        vc->cells[terminal_rows_count - 1][col].glyph = ' ';
        vc->cells[terminal_rows_count - 1][col].color = vc->color;
    }

    if (visible) {
        backend->scroll(vc->color);

        for (int col = 0; col < terminal_cols; col++) {
            render_cell_vc(vc, terminal_rows_count - 1, col);
        }
    }

    vc->cursor_row = terminal_rows_count - 1;
    vc->cursor_col = 0;
}

void terminal_set_color(unsigned char color) {
    output_vc()->color = color;
}

void terminal_set_overwrite_mode(int enabled) {
    terminal_overwrite_cursor = enabled ? 1 : 0;

    if (cursor_visible) {
        cursor_visible = 0;
        show_cursor();
    }
}

int terminal_overwrite_mode(void) {
    return terminal_overwrite_cursor;
}

void terminal_capture_begin(char* buffer, unsigned int buffer_size) {
    terminal_capture_buffer = buffer;
    terminal_capture_size = buffer_size;
    terminal_capture_length = 0;
    terminal_capture_active = (buffer != 0 && buffer_size > 0) ? 1 : 0;
    terminal_capture_dynamic = 0;

    if (terminal_capture_active) {
        terminal_capture_buffer[0] = '\0';
    }
}

unsigned int terminal_capture_end(void) {
    unsigned int length = terminal_capture_length;

    terminal_capture_buffer = 0;
    terminal_capture_size = 0;
    terminal_capture_length = 0;
    terminal_capture_active = 0;
    terminal_capture_dynamic = 0;

    return length;
}

int terminal_capture_begin_dynamic(unsigned int initial_size) {
    char* buffer;
    unsigned int size = initial_size > 0 ? initial_size : 64U;

    buffer = (char*)kmalloc(size);
    if (buffer == 0) {
        terminal_capture_buffer = 0;
        terminal_capture_size = 0;
        terminal_capture_length = 0;
        terminal_capture_active = 0;
        terminal_capture_dynamic = 0;
        return 0;
    }

    terminal_capture_buffer = buffer;
    terminal_capture_size = size;
    terminal_capture_length = 0;
    terminal_capture_active = 1;
    terminal_capture_dynamic = 1;
    terminal_capture_buffer[0] = '\0';
    return 1;
}

char* terminal_capture_end_dynamic(unsigned int* length) {
    char* buffer;

    if (length != 0) {
        *length = terminal_capture_length;
    }

    buffer = terminal_capture_buffer;
    terminal_capture_buffer = 0;
    terminal_capture_size = 0;
    terminal_capture_length = 0;
    terminal_capture_active = 0;
    terminal_capture_dynamic = 0;
    return buffer;
}

void terminal_get_cursor(int* row, int* col) {
    const terminal_vc_state_t* vc = output_vc();

    if (row != 0) {
        *row = vc->cursor_row;
    }

    if (col != 0) {
        *col = vc->cursor_col;
    }
}

void terminal_set_cursor(int row, int col) {
    terminal_vc_state_t* vc = output_vc();
    int visible = output_is_visible();

    sync_terminal_geometry();

    if (row < 0) {
        row = 0;
    }

    if (col < 0) {
        col = 0;
    }

    if (row >= terminal_rows_count) {
        row = terminal_rows_count - 1;
    }

    if (col >= terminal_cols) {
        col = terminal_cols - 1;
    }

    if (visible) hide_cursor();

    vc->cursor_row = row;
    vc->cursor_col = col;
    if (visible) {
        show_cursor();
        last_blink = timer_get_ticks();
    }
}

void terminal_put_char_with_color(char c, unsigned char color) {
    terminal_vc_state_t* vc = output_vc();
    unsigned char previous_color = vc->color;

    vc->color = color;
    terminal_put_char(c);
    vc->color = previous_color;
}

void terminal_clear(void) {
    const console_backend_t* backend;
    terminal_vc_state_t* vc = output_vc();
    int visible = output_is_visible();

    sync_terminal_geometry();
    backend = terminal_backend();
    if (visible) {
        backend->clear(vc->color);
    }
    reset_screen_buffer(vc);

    vc->cursor_row = 0;
    vc->cursor_col = 0;
    vc->utf8_accum = 0;
    vc->utf8_remain = 0;
    if (visible) {
        cursor_visible = 0;
        last_blink = timer_get_ticks();
        show_cursor();
    }
}

void terminal_put_char(char c) {
    terminal_vc_state_t* vc = output_vc();
    int visible = output_is_visible();
    unsigned char b = (unsigned char)c;
    uint32_t cp;
    unsigned int glyph;

    /* Capture mode: pass raw bytes through (keeps valid UTF-8 in the buffer) */
    if (terminal_capture_active) {
        terminal_capture_put_char(c);
        return;
    }

#if LYTH_AUTOTEST_ENABLED
    serial_putc(c);
#endif

    sync_terminal_geometry();

    /* ---- UTF-8 state machine ---- */
    if (b < 0x80) {
        /* ASCII — always resets any pending sequence */
        vc->utf8_remain = 0;
        cp = b;
    } else if (b >= 0xC0 && b <= 0xDF) {
        vc->utf8_accum  = (uint32_t)(b & 0x1F);
        vc->utf8_remain = 1;
        return;
    } else if (b >= 0xE0 && b <= 0xEF) {
        vc->utf8_accum  = (uint32_t)(b & 0x0F);
        vc->utf8_remain = 2;
        return;
    } else if (b >= 0xF0 && b <= 0xF7) {
        vc->utf8_accum  = (uint32_t)(b & 0x07);
        vc->utf8_remain = 3;
        return;
    } else if (b >= 0x80 && b <= 0xBF && vc->utf8_remain > 0) {
        vc->utf8_accum = (vc->utf8_accum << 6) | (uint32_t)(b & 0x3F);
        vc->utf8_remain--;
        if (vc->utf8_remain > 0) return;   /* more bytes needed */
        cp = vc->utf8_accum;
        vc->utf8_accum = 0;
    } else {
        /* Invalid byte — emit replacement and reset */
        vc->utf8_remain = 0;
        cp = '?';
    }

    glyph = unicode_to_cp437(cp);

    if (visible) hide_cursor();

    if (cp == '\n') {
        vc->cursor_row++;
        vc->cursor_col = 0;
        scroll_if_needed();
        if (visible) {
            show_cursor();
            last_blink = timer_get_ticks();
        }
        return;
    }

    vc->cells[vc->cursor_row][vc->cursor_col].glyph = glyph;
    vc->cells[vc->cursor_row][vc->cursor_col].color = vc->color;
    if (visible) render_cell_vc(vc, vc->cursor_row, vc->cursor_col);

    vc->cursor_col++;

    if (vc->cursor_col >= terminal_cols) {
        vc->cursor_col = 0;
        vc->cursor_row++;
        scroll_if_needed();
    }

    if (visible) {
        show_cursor();
        last_blink = timer_get_ticks();
    }
}

void terminal_backspace(void) {
    terminal_vc_state_t* vc = output_vc();
    int visible = output_is_visible();

    sync_terminal_geometry();

    if (terminal_capture_active) {
        if (terminal_capture_length > 0) {
            terminal_capture_length--;
            terminal_capture_buffer[terminal_capture_length] = '\0';
        }
        return;
    }

    if (visible) hide_cursor();

    if (vc->cursor_col > 0) {
        vc->cursor_col--;
        vc->cells[vc->cursor_row][vc->cursor_col].glyph = ' ';
        vc->cells[vc->cursor_row][vc->cursor_col].color = vc->color;
        if (visible) render_cell_vc(vc, vc->cursor_row, vc->cursor_col);
    }

    if (visible) {
        show_cursor();
        last_blink = timer_get_ticks();
    }
}
void terminal_write_uint(unsigned int value) {
    char buffer[16];
    int index = 0;

    if (value == 0) {
        terminal_put_char('0');
        return;
    }

    while (value > 0) {
        buffer[index++] = (char)('0' + (value % 10));
        value /= 10;
    }

    while (index > 0) {
        terminal_put_char(buffer[--index]);
    }
}

void terminal_print(const char* str) {
    int i = 0;
    while (str[i] != '\0') {
        terminal_put_char(str[i]);
        i++;
    }
}

void terminal_print_line(const char* str) {
    terminal_print(str);
    terminal_put_char('\n');
}

void terminal_print_uint(unsigned int value) {
    char digits[16];
    int length = 0;

    if (value == 0) {
        terminal_put_char('0');
        return;
    }

    while (value > 0 && length < (int)sizeof(digits)) {
        digits[length] = (char)('0' + (value % 10));
        value /= 10;
        length++;
    }

    while (length > 0) {
        length--;
        terminal_put_char(digits[length]);
    }
}

void terminal_print_hex(unsigned int value) {
    static const char hex_digits[] = "0123456789ABCDEF";

    terminal_print("0x");

    for (int shift = 28; shift >= 0; shift -= 4) {
        terminal_put_char(hex_digits[(value >> shift) & 0x0F]);
    }
}

void terminal_init(void) {
    sync_terminal_geometry();

    for (int index = 0; index < TERMINAL_VC_COUNT; index++) {
        terminal_vcs[index].color = 0x0F;
        terminal_vcs[index].cursor_row = 0;
        terminal_vcs[index].cursor_col = 0;
        terminal_vcs[index].utf8_accum = 0;
        terminal_vcs[index].utf8_remain = 0;
        reset_screen_buffer(&terminal_vcs[index]);
    }

    terminal_active_console = 0;
    terminal_clear();
}

int terminal_rows(void) {
    sync_terminal_geometry();
    return terminal_rows_count;
}

int terminal_columns(void) {
    sync_terminal_geometry();
    return terminal_cols;
}

int terminal_switch_vc(int index) {
    terminal_vc_state_t* vc;

    if (index < 0 || index >= TERMINAL_VC_COUNT || index == terminal_active_console) {
        return (index == terminal_active_console) ? 1 : 0;
    }

    sync_terminal_geometry();
    hide_cursor();
    terminal_active_console = index;
    vc = active_vc();

    if (vc->cursor_row >= terminal_rows_count) {
        vc->cursor_row = terminal_rows_count - 1;
    }
    if (vc->cursor_col >= terminal_cols) {
        vc->cursor_col = terminal_cols - 1;
    }

    render_active_console();
    cursor_visible = 0;
    show_cursor();
    last_blink = timer_get_ticks();
    return 1;
}

int terminal_active_vc(void) {
    return terminal_active_console;
}

int terminal_vc_count(void) {
    return TERMINAL_VC_COUNT;
}

void terminal_set_output_vc(int vc_index) {
    terminal_output_vc_override = vc_index;
}