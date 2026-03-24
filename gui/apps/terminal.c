/*
 * Terminal app — embedded console window for the GUI desktop.
 *
 * Renders a character grid into the window's surface. Supports
 * basic text output, scrolling, and keyboard input with a command
 * prompt. This is a minimal shell that responds to simple built-in
 * commands and shows system output.
 */

#include "terminal.h"
#include "compositor.h"
#include "window.h"
#include "input.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "physmem.h"
#include "task.h"
#include "heap.h"

#define TERM_COLS    80
#define TERM_ROWS    24
#define TERM_BUF_ROWS 256   /* scrollback buffer */
#define TERM_MAX_CMD 128

#define COL_TERM_BG   0x1E1E2E
#define COL_TERM_FG   0xCDD6F4
#define COL_TERM_PROMPT 0x89B4FA
#define COL_TERM_CURSOR 0xF5C2E7
#define COL_TERM_ERR  0xF38BA8

typedef struct {
    char cells[TERM_BUF_ROWS][TERM_COLS];
    uint32_t colors[TERM_BUF_ROWS][TERM_COLS];
    int cur_row, cur_col;
    int scroll_top;    /* first visible row in buffer */
    int buf_row;       /* current write row in buffer */
    char cmd[TERM_MAX_CMD];
    int cmd_len;
    int is_open;
} term_state_t;

static term_state_t term;

/* forward */
static void term_paint(gui_window_t* win);
static void term_on_key(gui_window_t* win, int event_type, char key);
static void term_on_close(gui_window_t* win);
static void term_putchar(char c, uint32_t color);
static void term_puts(const char* s, uint32_t color);
static void term_newline(void);
static void term_prompt(void);
static void term_exec(const char* cmd);
static void term_scroll_if_needed(void);

static gui_window_t* term_window;

/* ------------------------------------------------------------------ */

static void term_clear(void) {
    int r, c;
    for (r = 0; r < TERM_BUF_ROWS; r++) {
        for (c = 0; c < TERM_COLS; c++) {
            term.cells[r][c] = ' ';
            term.colors[r][c] = COL_TERM_FG;
        }
    }
    term.cur_row = 0;
    term.cur_col = 0;
    term.scroll_top = 0;
    term.buf_row = 0;
    term.cmd_len = 0;
}

static void term_scroll_if_needed(void) {
    if (term.buf_row - term.scroll_top >= TERM_ROWS) {
        term.scroll_top = term.buf_row - TERM_ROWS + 1;
    }
}

static void term_newline(void) {
    term.cur_col = 0;
    term.buf_row++;
    if (term.buf_row >= TERM_BUF_ROWS) {
        /* wrap around: shift everything up */
        int r, c;
        for (r = 0; r < TERM_BUF_ROWS - 1; r++) {
            memcpy(term.cells[r], term.cells[r + 1], TERM_COLS);
            memcpy(term.colors[r], term.colors[r + 1], TERM_COLS * 4);
        }
        for (c = 0; c < TERM_COLS; c++) {
            term.cells[TERM_BUF_ROWS - 1][c] = ' ';
            term.colors[TERM_BUF_ROWS - 1][c] = COL_TERM_FG;
        }
        term.buf_row = TERM_BUF_ROWS - 1;
        if (term.scroll_top > 0) term.scroll_top--;
    }
    term_scroll_if_needed();
}

static void term_putchar(char c, uint32_t color) {
    if (c == '\n') {
        term_newline();
        return;
    }
    if (term.cur_col >= TERM_COLS) {
        term_newline();
    }
    term.cells[term.buf_row][term.cur_col] = c;
    term.colors[term.buf_row][term.cur_col] = color;
    term.cur_col++;
}

static void term_puts(const char* s, uint32_t color) {
    while (*s) {
        term_putchar(*s, color);
        s++;
    }
}

static void term_prompt(void) {
    term_puts("lyth", COL_TERM_PROMPT);
    term_puts("> ", COL_TERM_FG);
    term.cmd_len = 0;
}

/* ---- built-in commands ---- */

static void cmd_help(void) {
    term_puts("Available commands:\n", COL_TERM_FG);
    term_puts("  help      - show this message\n", COL_TERM_FG);
    term_puts("  clear     - clear terminal\n", COL_TERM_FG);
    term_puts("  uptime    - show system uptime\n", COL_TERM_FG);
    term_puts("  mem       - show memory usage\n", COL_TERM_FG);
    term_puts("  ps        - list processes\n", COL_TERM_FG);
    term_puts("  uname     - system info\n", COL_TERM_FG);
    term_puts("  echo <t>  - print text\n", COL_TERM_FG);
}

static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static void cmd_uptime(void) {
    unsigned int ms = timer_get_uptime_ms();
    unsigned int secs = ms / 1000;
    unsigned int mins = secs / 60;
    unsigned int hrs = mins / 60;
    char buf[32];

    term_puts("Uptime: ", COL_TERM_FG);
    int_to_str(hrs, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts("h ", COL_TERM_FG);
    int_to_str(mins % 60, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts("m ", COL_TERM_FG);
    int_to_str(secs % 60, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts("s\n", COL_TERM_FG);
}

static void cmd_mem(void) {
    char buf[16];
    unsigned int total = physmem_total_bytes() / 1024;
    unsigned int free_b = physmem_free_bytes() / 1024;
    unsigned int used = total - free_b;

    term_puts("Memory: ", COL_TERM_FG);
    int_to_str(used, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts(" KB used / ", COL_TERM_FG);
    int_to_str(total, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts(" KB total (", COL_TERM_FG);
    int_to_str(free_b, buf, sizeof(buf)); term_puts(buf, COL_TERM_FG);
    term_puts(" KB free)\n", COL_TERM_FG);
}

static void cmd_ps(void) {
    task_snapshot_t snaps[32];
    int count = task_list(snaps, 32);
    int i;
    char buf[8];

    term_puts("PID  STATE        NAME\n", COL_TERM_FG);
    for (i = 0; i < count; i++) {
        int_to_str(snaps[i].id, buf, sizeof(buf));
        int pad = 5 - (int)strlen(buf);
        term_puts(buf, COL_TERM_FG);
        while (pad-- > 0) term_putchar(' ', COL_TERM_FG);

        const char* st = task_state_name(snaps[i].state);
        int slen = (int)strlen(st);
        term_puts(st, COL_TERM_FG);
        pad = 13 - slen;
        while (pad-- > 0) term_putchar(' ', COL_TERM_FG);

        term_puts(snaps[i].name, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
    }
}

static void cmd_uname(void) {
    term_puts("Lyth OS x86_64\n", COL_TERM_FG);
}

static void term_exec(const char* cmd) {
    /* skip leading spaces */
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    if (str_equals(cmd, "help")) { cmd_help(); return; }
    if (str_equals(cmd, "clear")) {
        term_clear();
        term_puts("Lyth Terminal\n\n", COL_TERM_PROMPT);
        return;
    }
    if (str_equals(cmd, "uptime")) { cmd_uptime(); return; }
    if (str_equals(cmd, "mem")) { cmd_mem(); return; }
    if (str_equals(cmd, "ps")) { cmd_ps(); return; }
    if (str_equals(cmd, "uname")) { cmd_uname(); return; }
    if (str_starts_with(cmd, "echo ")) {
        term_puts(cmd + 5, COL_TERM_FG);
        term_putchar('\n', COL_TERM_FG);
        return;
    }

    term_puts("Unknown command: ", COL_TERM_ERR);
    term_puts(cmd, COL_TERM_ERR);
    term_putchar('\n', COL_TERM_FG);
}

/* ---- window callbacks ---- */

static void term_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cw, ch, ox, oy, row, col;

    if (!s->pixels) return;

    /* clear window surface */
    gui_surface_clear(s, COL_TERM_BG);

    /* draw title bar (if decorated) */
    if (!(win->flags & GUI_WIN_NO_DECOR)) {
        /* title bar bg */
        gui_surface_fill(s, 0, 0, win->width, GUI_TITLEBAR_HEIGHT, 0x181825);
        /* close button circle */
        {
            int cx = win->width - 20;
            int cy = GUI_TITLEBAR_HEIGHT / 2;
            int r;
            for (r = -5; r <= 5; r++) {
                int dx;
                for (dx = -5; dx <= 5; dx++) {
                    if (r * r + dx * dx <= 25)
                        gui_surface_putpixel(s, cx + dx, cy + r, 0xF38BA8);
                }
            }
        }
        /* title text */
        gui_surface_draw_string(s, 10, (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2,
                                win->title, 0xCDD6F4, 0, 0);
        /* separator line */
        gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, win->width, 0x313244);
    }

    ox = GUI_BORDER_WIDTH + 4;
    oy = (win->flags & GUI_WIN_NO_DECOR) ? 4 : GUI_TITLEBAR_HEIGHT + 2;
    cw = (win->width - ox * 2) / FONT_PSF_WIDTH;
    ch = (win->height - oy - 4) / FONT_PSF_HEIGHT;
    if (cw > TERM_COLS) cw = TERM_COLS;
    if (ch > TERM_ROWS) ch = TERM_ROWS;

    /* draw visible rows from scrollback */
    for (row = 0; row < ch; row++) {
        int buf_r = term.scroll_top + row;
        if (buf_r < 0 || buf_r >= TERM_BUF_ROWS) continue;
        for (col = 0; col < cw; col++) {
            char c = term.cells[buf_r][col];
            if (c > ' ') {
                gui_surface_draw_char(s, ox + col * FONT_PSF_WIDTH,
                                      oy + row * FONT_PSF_HEIGHT,
                                      (unsigned char)c,
                                      term.colors[buf_r][col], 0, 0);
            }
        }
    }

    /* cursor */
    {
        int curs_row = term.buf_row - term.scroll_top;
        if (curs_row >= 0 && curs_row < ch) {
            gui_surface_fill(s, ox + term.cur_col * FONT_PSF_WIDTH,
                             oy + curs_row * FONT_PSF_HEIGHT + FONT_PSF_HEIGHT - 2,
                             FONT_PSF_WIDTH, 2, COL_TERM_CURSOR);
        }
    }
}

static void term_on_key(gui_window_t* win, int event_type, char key) {
    if (event_type == INPUT_EVENT_CHAR && key >= 32 && key < 127) {
        if (term.cmd_len < TERM_MAX_CMD - 1) {
            term.cmd[term.cmd_len++] = key;
            term_putchar(key, COL_TERM_FG);
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
        }
    } else if (event_type == INPUT_EVENT_ENTER) {
        term.cmd[term.cmd_len] = '\0';
        term_newline();
        term_exec(term.cmd);
        term_prompt();
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    } else if (event_type == INPUT_EVENT_BACKSPACE) {
        if (term.cmd_len > 0) {
            term.cmd_len--;
            if (term.cur_col > 0) {
                term.cur_col--;
                term.cells[term.buf_row][term.cur_col] = ' ';
            }
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
        }
    }
}

static void term_on_close(gui_window_t* win) {
    term.is_open = 0;
    term_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- public ---- */

void terminal_app_open(void) {
    int w, h;

    if (term.is_open && term_window) {
        gui_window_focus(term_window);
        gui_dirty_add(term_window->x, term_window->y,
                      term_window->width, term_window->height);
        return;
    }

    w = TERM_COLS * FONT_PSF_WIDTH + 16;
    h = TERM_ROWS * FONT_PSF_HEIGHT + GUI_TITLEBAR_HEIGHT + 12;

    term_window = gui_window_create("Terminal", 60, 40, w, h,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!term_window) return;

    term_window->on_paint = term_paint;
    term_window->on_key = term_on_key;
    term_window->on_close = term_on_close;

    term_clear();
    term_puts("Lyth Terminal\n", COL_TERM_PROMPT);
    term_puts("Type 'help' for available commands.\n\n", COL_TERM_FG);
    term_prompt();

    term.is_open = 1;
    gui_window_focus(term_window);
    gui_dirty_add(term_window->x, term_window->y,
                  term_window->width, term_window->height);
}
