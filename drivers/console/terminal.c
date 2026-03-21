#include "terminal.h"
#include "console_backend.h"
#include "timer.h"
#include "utf8.h"
#include <stdint.h>

#define TERMINAL_MAX_COLUMNS 240
#define TERMINAL_MAX_ROWS 80

typedef struct {
    unsigned int glyph;  /* CP437 font glyph index */
    unsigned char color;
} terminal_cell_t;

static unsigned char terminal_color = 0x0F;
static int terminal_overwrite_cursor = 0;
static char* terminal_capture_buffer = 0;
static unsigned int terminal_capture_size = 0;
static unsigned int terminal_capture_length = 0;
static int terminal_capture_active = 0;

static terminal_cell_t terminal_cells[TERMINAL_MAX_ROWS][TERMINAL_MAX_COLUMNS];
static int terminal_cols = 80;
static int terminal_rows_count = 25;

static int cursor_row = 0;
static int cursor_col = 0;

static int cursor_visible = 0;
static uint32_t last_blink = 0;

/* UTF-8 multi-byte state machine */
static uint32_t utf8_accum   = 0;  /* accumulated codepoint bits */
static int      utf8_remain  = 0;  /* continuation bytes still needed */

static int clamp_positive(int value, int fallback) {
    return value > 0 ? value : fallback;
}

static const console_backend_t* terminal_backend(void) {
    return console_backend_current();
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

static void render_cell(int row, int col) {
    const console_backend_t* backend = terminal_backend();

    if (row < 0 || row >= terminal_rows_count || col < 0 || col >= terminal_cols) {
        return;
    }

    backend->put_cell(
        row,
        col,
        terminal_cells[row][col].glyph,
        terminal_cells[row][col].color
    );
}

static void terminal_capture_put_char(char c) {
    if (!terminal_capture_active || terminal_capture_buffer == 0 || terminal_capture_size == 0) {
        return;
    }

    if (terminal_capture_length >= terminal_capture_size - 1) {
        terminal_capture_buffer[terminal_capture_size - 1] = '\0';
        return;
    }

    terminal_capture_buffer[terminal_capture_length++] = c;
    terminal_capture_buffer[terminal_capture_length] = '\0';
}

static void hide_cursor(void) {
    const console_backend_t* backend = terminal_backend();

    if (!backend->software_cursor || !cursor_visible) {
        return;
    }

    render_cell(cursor_row, cursor_col);
    cursor_visible = 0;
}

static void show_cursor(void) {
    const console_backend_t* backend = terminal_backend();

    backend->show_cursor(cursor_row, cursor_col, terminal_color);

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

static void reset_screen_buffer(void) {
    for (int row = 0; row < terminal_rows_count; row++) {
        for (int col = 0; col < terminal_cols; col++) {
            terminal_cells[row][col].glyph = ' ';
            terminal_cells[row][col].color = terminal_color;
        }
    }
}

static void scroll_if_needed(void) {
    const console_backend_t* backend = terminal_backend();

    if (cursor_row < terminal_rows_count) {
        return;
    }

    for (int row = 1; row < terminal_rows_count; row++) {
        for (int col = 0; col < terminal_cols; col++) {
            terminal_cells[row - 1][col] = terminal_cells[row][col];
        }
    }

    for (int col = 0; col < terminal_cols; col++) {
        terminal_cells[terminal_rows_count - 1][col].glyph = ' ';
        terminal_cells[terminal_rows_count - 1][col].color = terminal_color;
    }

    backend->scroll(terminal_color);

    for (int col = 0; col < terminal_cols; col++) {
        render_cell(terminal_rows_count - 1, col);
    }

    cursor_row = terminal_rows_count - 1;
    cursor_col = 0;
}

void terminal_set_color(unsigned char color) {
    terminal_color = color;
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

    return length;
}

void terminal_get_cursor(int* row, int* col) {
    if (row != 0) {
        *row = cursor_row;
    }

    if (col != 0) {
        *col = cursor_col;
    }
}

void terminal_set_cursor(int row, int col) {
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

    hide_cursor();

    cursor_row = row;
    cursor_col = col;
    show_cursor();
    last_blink = timer_get_ticks();
}

void terminal_put_char_with_color(char c, unsigned char color) {
    unsigned char previous_color = terminal_color;

    terminal_color = color;
    terminal_put_char(c);
    terminal_color = previous_color;
}

void terminal_clear(void) {
    const console_backend_t* backend;

    sync_terminal_geometry();
    backend = terminal_backend();
    backend->clear(terminal_color);
    reset_screen_buffer();

    cursor_row = 0;
    cursor_col = 0;
    cursor_visible = 0;
    last_blink = timer_get_ticks();
    show_cursor();
}

void terminal_put_char(char c) {
    unsigned char b = (unsigned char)c;
    uint32_t cp;
    unsigned int glyph;

    /* Capture mode: pass raw bytes through (keeps valid UTF-8 in the buffer) */
    if (terminal_capture_active) {
        terminal_capture_put_char(c);
        return;
    }

    sync_terminal_geometry();

    /* ---- UTF-8 state machine ---- */
    if (b < 0x80) {
        /* ASCII — always resets any pending sequence */
        utf8_remain = 0;
        cp = b;
    } else if (b >= 0xC0 && b <= 0xDF) {
        utf8_accum  = (uint32_t)(b & 0x1F);
        utf8_remain = 1;
        return;
    } else if (b >= 0xE0 && b <= 0xEF) {
        utf8_accum  = (uint32_t)(b & 0x0F);
        utf8_remain = 2;
        return;
    } else if (b >= 0xF0 && b <= 0xF7) {
        utf8_accum  = (uint32_t)(b & 0x07);
        utf8_remain = 3;
        return;
    } else if (b >= 0x80 && b <= 0xBF && utf8_remain > 0) {
        utf8_accum = (utf8_accum << 6) | (uint32_t)(b & 0x3F);
        utf8_remain--;
        if (utf8_remain > 0) return;   /* more bytes needed */
        cp = utf8_accum;
        utf8_accum = 0;
    } else {
        /* Invalid byte — emit replacement and reset */
        utf8_remain = 0;
        cp = '?';
    }

    glyph = unicode_to_cp437(cp);

    hide_cursor();

    if (cp == '\n') {
        cursor_row++;
        cursor_col = 0;
        scroll_if_needed();
        show_cursor();
        last_blink = timer_get_ticks();
        return;
    }

    terminal_cells[cursor_row][cursor_col].glyph = glyph;
    terminal_cells[cursor_row][cursor_col].color = terminal_color;
    render_cell(cursor_row, cursor_col);

    cursor_col++;

    if (cursor_col >= terminal_cols) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }

    show_cursor();
    last_blink = timer_get_ticks();
}

void terminal_backspace(void) {
    sync_terminal_geometry();

    if (terminal_capture_active) {
        if (terminal_capture_length > 0) {
            terminal_capture_length--;
            terminal_capture_buffer[terminal_capture_length] = '\0';
        }
        return;
    }

    hide_cursor();

    if (cursor_col > 0) {
        cursor_col--;
        terminal_cells[cursor_row][cursor_col].glyph = ' ';
        terminal_cells[cursor_row][cursor_col].color = terminal_color;
        render_cell(cursor_row, cursor_col);
    }

    show_cursor();
    last_blink = timer_get_ticks();
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