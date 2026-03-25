/* ============================================================
 *  editor.c  —  Lyth Notes (simple text editor)
 *
 *  Open, edit, save plain-text files via VFS.
 *  Keyboard-driven with Ctrl shortcuts.
 * ============================================================ */

#include "editor.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "vfs.h"
#include "timer.h"
#include "input.h"

/* ---- Colours (from theme.h) ---- */
#define COL_ED_BG       THEME_COL_BASE
#define COL_ED_TEXT      THEME_COL_TEXT
#define COL_ED_DIM       THEME_COL_DIM
#define COL_ED_ACCENT    THEME_COL_ACCENT
#define COL_ED_GUTTER    THEME_COL_MANTLE
#define COL_ED_LINENUM   THEME_COL_SURFACE2
#define COL_ED_CURSOR    THEME_COL_CURSOR
#define COL_ED_SELECT    THEME_COL_SURFACE1
#define COL_ED_STATUS    THEME_COL_CRUST
#define COL_ED_STATUS_FG THEME_COL_SUBTEXT0
#define COL_ED_MODIFIED  THEME_COL_WARNING
#define COL_ED_BORDER    THEME_COL_BORDER
#define COL_ED_ERR       THEME_COL_ERROR
#define COL_ED_OK        THEME_COL_SUCCESS

/* ---- Layout ---- */
#define ED_WIN_W      560
#define ED_WIN_H      440
#define ED_GUTTER_W   40
#define ED_PAD        4
#define ED_STATUS_H   20

/* ---- Buffer limits ---- */
#define ED_MAX_CHARS  8192
#define ED_MAX_LINES  512
#define ED_LINE_MAX   256
#define ED_PATH_MAX   256

/* ---- State ---- */
static gui_window_t* ed_window;
static int ed_is_open;

static char ed_buf[ED_MAX_CHARS];
static int  ed_buf_len;

/* Cursor position (line, col) */
static int ed_cur_line;
static int ed_cur_col;

/* Scroll */
static int ed_scroll_y;

/* File path */
static char ed_filepath[ED_PATH_MAX];
static int  ed_modified;

/* Status message */
static char ed_status[48];
static uint32_t ed_status_color;
static unsigned int ed_status_expire;

/* ---- Helpers ---- */

static void ed_set_status(const char* msg, uint32_t color) {
    int i = 0;
    while (msg[i] && i + 1 < (int)sizeof(ed_status)) {
        ed_status[i] = msg[i];
        i++;
    }
    ed_status[i] = '\0';
    ed_status_color = color;
    ed_status_expire = timer_get_uptime_ms() + 3000;
}

/* Count lines in buffer */
static int ed_line_count(void) {
    int count = 1;
    int i;
    for (i = 0; i < ed_buf_len; i++) {
        if (ed_buf[i] == '\n') count++;
    }
    return count;
}

/* Get offset of start of line N (0-based) */
static int ed_line_start(int line) {
    int cur = 0, i;
    for (i = 0; i < ed_buf_len && cur < line; i++) {
        if (ed_buf[i] == '\n') cur++;
    }
    return (cur == line) ? i : ed_buf_len;
}

/* Get length of line N (excluding newline) */
static int ed_line_length(int line) {
    int start = ed_line_start(line);
    int len = 0;
    while (start + len < ed_buf_len && ed_buf[start + len] != '\n')
        len++;
    return len;
}

/* Get character offset of cursor */
static int ed_cursor_offset(void) {
    int start = ed_line_start(ed_cur_line);
    int col = ed_cur_col;
    int line_len = ed_line_length(ed_cur_line);
    if (col > line_len) col = line_len;
    return start + col;
}

static int ed_visible_lines(void) {
    int content_h = ED_WIN_H - GUI_TITLEBAR_HEIGHT - ED_STATUS_H;
    return content_h / GUI_FONT_H;
}

/* Clamp cursor to valid position */
static void ed_clamp_cursor(void) {
    int lines = ed_line_count();
    if (ed_cur_line >= lines) ed_cur_line = lines - 1;
    if (ed_cur_line < 0) ed_cur_line = 0;
    int len = ed_line_length(ed_cur_line);
    if (ed_cur_col > len) ed_cur_col = len;
    if (ed_cur_col < 0) ed_cur_col = 0;
}

/* Ensure cursor is visible */
static void ed_scroll_to_cursor(void) {
    int vis = ed_visible_lines();
    if (ed_cur_line < ed_scroll_y) ed_scroll_y = ed_cur_line;
    if (ed_cur_line >= ed_scroll_y + vis) ed_scroll_y = ed_cur_line - vis + 1;
    if (ed_scroll_y < 0) ed_scroll_y = 0;
}

/* ---- File I/O ---- */

static void ed_load_file(const char* path) {
    int fd, n;

    str_copy(ed_filepath, path, ED_PATH_MAX);
    ed_buf[0] = '\0';
    ed_buf_len = 0;
    ed_cur_line = 0;
    ed_cur_col = 0;
    ed_scroll_y = 0;
    ed_modified = 0;

    fd = vfs_open(path);
    if (fd < 0) {
        ed_set_status("New file", COL_ED_ACCENT);
        return;
    }

    n = vfs_read(fd, (unsigned char*)ed_buf, ED_MAX_CHARS - 1);
    if (n > 0) ed_buf_len = n;
    ed_buf[ed_buf_len] = '\0';
    vfs_close(fd);

    ed_set_status("Opened", COL_ED_OK);
}

static void ed_save_file(void) {
    int fd;

    if (ed_filepath[0] == '\0') {
        ed_set_status("No filename (use Ctrl+N)", COL_ED_ERR);
        return;
    }

    fd = vfs_open_flags(ed_filepath, VFS_O_WRONLY | VFS_O_CREAT | VFS_O_TRUNC);
    if (fd < 0) {
        ed_set_status("Cannot save", COL_ED_ERR);
        return;
    }

    vfs_write(fd, (const unsigned char*)ed_buf, (unsigned int)ed_buf_len);
    vfs_close(fd);

    ed_modified = 0;
    ed_set_status("Saved", COL_ED_OK);
}

/* ---- Text editing operations ---- */

static void ed_insert_char(char c) {
    int off = ed_cursor_offset();
    if (ed_buf_len >= ED_MAX_CHARS - 1) return;

    /* Shift right */
    {
        int i;
        for (i = ed_buf_len; i > off; i--)
            ed_buf[i] = ed_buf[i - 1];
    }
    ed_buf[off] = c;
    ed_buf_len++;
    ed_buf[ed_buf_len] = '\0';

    if (c == '\n') {
        ed_cur_line++;
        ed_cur_col = 0;
    } else {
        ed_cur_col++;
    }
    ed_modified = 1;
}

static void ed_delete_back(void) {
    int off = ed_cursor_offset();
    if (off <= 0) return;

    /* Check if deleting a newline */
    if (ed_buf[off - 1] == '\n') {
        int prev_len = ed_line_length(ed_cur_line - 1);
        ed_cur_line--;
        ed_cur_col = prev_len;
    } else {
        ed_cur_col--;
    }

    /* Shift left */
    {
        int i;
        for (i = off - 1; i < ed_buf_len - 1; i++)
            ed_buf[i] = ed_buf[i + 1];
    }
    ed_buf_len--;
    ed_buf[ed_buf_len] = '\0';
    ed_modified = 1;
}

static void ed_delete_forward(void) {
    int off = ed_cursor_offset();
    if (off >= ed_buf_len) return;

    {
        int i;
        for (i = off; i < ed_buf_len - 1; i++)
            ed_buf[i] = ed_buf[i + 1];
    }
    ed_buf_len--;
    ed_buf[ed_buf_len] = '\0';
    ed_modified = 1;
}

/* ---- Drawing ---- */

static void ed_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int w = win->width;
    int y, i, vis;

    if (!s->pixels) return;

    gui_surface_clear(s, COL_ED_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    /* Extra title info (modified flag + filename) after decorations */
    if (ed_modified || ed_filepath[0]) {
        int text_y = (GUI_TITLEBAR_HEIGHT - GUI_FONT_H) / 2;
        int title_len = 0;
        { const char* p = win->title; while (*p) { title_len++; p++; } }
        int title_w = title_len * GUI_FONT_W;
        /* center offset matching decoration title position */
        int title_x = (w - title_w) / 2;
        if (title_x < 76) title_x = 76;
        int tx = title_x + title_w;
        if (ed_modified) {
            gui_surface_draw_string(s, tx, text_y, " *", COL_ED_MODIFIED, 0, 0);
            tx += 2 * GUI_FONT_W;
        }
        if (ed_filepath[0]) {
            const char* base = ed_filepath;
            const char* p2 = ed_filepath;
            while (*p2) { if (*p2 == '/') base = p2 + 1; p2++; }
            if (*base) {
                gui_surface_draw_string(s, tx, text_y, " - ", COL_ED_DIM, 0, 0);
                gui_surface_draw_string(s, tx + 3 * GUI_FONT_W, text_y, base, COL_ED_DIM, 0, 0);
            }
        }
    }

    y = GUI_TITLEBAR_HEIGHT;
    vis = ed_visible_lines();

    /* Text area with line numbers */
    for (i = 0; i < vis; i++) {
        int line = ed_scroll_y + i;
        int lines = ed_line_count();
        int ly = y + i * GUI_FONT_H;

        /* Gutter */
        gui_surface_fill(s, 0, ly, ED_GUTTER_W, GUI_FONT_H, COL_ED_GUTTER);

        if (line < lines) {
            /* Line number */
            {
                char num[8];
                uint_to_str((unsigned int)(line + 1), num, sizeof(num));
                int nw = str_length(num) * GUI_FONT_W;
                gui_surface_draw_string(s, ED_GUTTER_W - nw - 4, ly,
                                        num, COL_ED_LINENUM, 0, 0);
            }

            /* Line text */
            {
                int start = ed_line_start(line);
                int len = ed_line_length(line);
                int max_chars = (w - ED_GUTTER_W - ED_PAD) / GUI_FONT_W;
                if (len > max_chars) len = max_chars;
                gui_surface_draw_string_n(s, ED_GUTTER_W + ED_PAD, ly,
                                          &ed_buf[start], len,
                                          COL_ED_TEXT, 0, 0);
            }

            /* Cursor */
            if (line == ed_cur_line) {
                int cx = ED_GUTTER_W + ED_PAD + ed_cur_col * GUI_FONT_W;
                gui_surface_fill(s, cx, ly, 2, GUI_FONT_H, COL_ED_CURSOR);
            }
        }

        /* Gutter separator */
        gui_surface_putpixel(s, ED_GUTTER_W - 1, ly, COL_ED_BORDER);
    }
    /* Gutter separator full line */
    {
        int r;
        for (r = y; r < ED_WIN_H - ED_STATUS_H; r++)
            gui_surface_putpixel(s, ED_GUTTER_W - 1, r, COL_ED_BORDER);
    }

    /* Status bar */
    {
        int sy = ED_WIN_H - ED_STATUS_H;
        gui_surface_fill(s, 0, sy, w, ED_STATUS_H, COL_ED_STATUS);
        gui_surface_hline(s, 0, sy, w, COL_ED_BORDER);

        /* Left: status or shortcuts */
        if (ed_status[0] && timer_get_uptime_ms() < ed_status_expire) {
            gui_surface_draw_string(s, ED_PAD, sy + 2, ed_status, ed_status_color, 0, 0);
        } else {
            gui_surface_draw_string(s, ED_PAD, sy + 2,
                                    "Ctrl+S:Save  Ctrl+N:New", COL_ED_DIM, 0, 0);
        }

        /* Right: Ln:Col */
        {
            char pos[24];
            str_copy(pos, "Ln ", sizeof(pos));
            {
                char n[8];
                uint_to_str((unsigned int)(ed_cur_line + 1), n, sizeof(n));
                str_append(pos, n, sizeof(pos));
            }
            str_append(pos, " Col ", sizeof(pos));
            {
                char n[8];
                uint_to_str((unsigned int)(ed_cur_col + 1), n, sizeof(n));
                str_append(pos, n, sizeof(pos));
            }
            int pw = str_length(pos) * GUI_FONT_W;
            gui_surface_draw_string(s, w - pw - ED_PAD, sy + 2, pos, COL_ED_STATUS_FG, 0, 0);
        }
    }
}

/* ---- Key handling ---- */

static void ed_on_key(gui_window_t* win, int event_type, char key) {
    (void)win;
    int lines;

    switch (event_type) {
    case INPUT_EVENT_UP:
        if (ed_cur_line > 0) ed_cur_line--;
        ed_clamp_cursor();
        break;

    case INPUT_EVENT_DOWN:
        lines = ed_line_count();
        if (ed_cur_line < lines - 1) ed_cur_line++;
        ed_clamp_cursor();
        break;

    case INPUT_EVENT_LEFT:
        if (ed_cur_col > 0) {
            ed_cur_col--;
        } else if (ed_cur_line > 0) {
            ed_cur_line--;
            ed_cur_col = ed_line_length(ed_cur_line);
        }
        break;

    case INPUT_EVENT_RIGHT: {
        int len = ed_line_length(ed_cur_line);
        if (ed_cur_col < len) {
            ed_cur_col++;
        } else if (ed_cur_line < ed_line_count() - 1) {
            ed_cur_line++;
            ed_cur_col = 0;
        }
        break;
    }

    case INPUT_EVENT_HOME:
        ed_cur_col = 0;
        break;

    case INPUT_EVENT_END:
        ed_cur_col = ed_line_length(ed_cur_line);
        break;

    case INPUT_EVENT_PAGE_UP:
        ed_cur_line -= ed_visible_lines();
        if (ed_cur_line < 0) ed_cur_line = 0;
        ed_clamp_cursor();
        break;

    case INPUT_EVENT_PAGE_DOWN:
        ed_cur_line += ed_visible_lines();
        lines = ed_line_count();
        if (ed_cur_line >= lines) ed_cur_line = lines - 1;
        ed_clamp_cursor();
        break;

    case INPUT_EVENT_ENTER:
        ed_insert_char('\n');
        break;

    case INPUT_EVENT_BACKSPACE:
        ed_delete_back();
        break;

    case INPUT_EVENT_DELETE:
        ed_delete_forward();
        break;

    case INPUT_EVENT_TAB:
        /* Insert 4 spaces */
        ed_insert_char(' ');
        ed_insert_char(' ');
        ed_insert_char(' ');
        ed_insert_char(' ');
        break;

    case INPUT_EVENT_CTRL_C:
        /* Ctrl+C — do nothing in editor */
        break;

    case INPUT_EVENT_CHAR:
        /* Ctrl+S = save (key 19 = 's' - 'a' + 1) */
        if (key == 19) {
            ed_save_file();
            break;
        }
        /* Ctrl+N = new file (key 14) */
        if (key == 14) {
            ed_buf[0] = '\0';
            ed_buf_len = 0;
            ed_filepath[0] = '\0';
            ed_cur_line = 0;
            ed_cur_col = 0;
            ed_scroll_y = 0;
            ed_modified = 0;
            ed_set_status("New file", COL_ED_ACCENT);
            break;
        }
        /* Printable character */
        if (key >= ' ' && key <= '~') {
            ed_insert_char(key);
        }
        break;

    default:
        break;
    }

    ed_scroll_to_cursor();
    gui_window_invalidate(ed_window);
}

/* ---- Click ---- */

static void ed_on_click(gui_window_t* win, int mx, int my, int button) {
    (void)win; (void)button;

    int y = GUI_TITLEBAR_HEIGHT;
    if (my < y) return;

    /* Click in text area */
    int line = ed_scroll_y + (my - y) / GUI_FONT_H;
    int lines = ed_line_count();
    if (line >= lines) line = lines - 1;
    if (line < 0) line = 0;
    ed_cur_line = line;

    int col = (mx - ED_GUTTER_W - ED_PAD) / GUI_FONT_W;
    if (col < 0) col = 0;
    ed_cur_col = col;
    ed_clamp_cursor();

    gui_window_invalidate(ed_window);
}

/* ---- Close ---- */

static void ed_on_close(gui_window_t* win) {
    ed_is_open = 0;
    ed_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- Public API ---- */

void editor_app_open(void) {
    editor_app_open_file("");
}

void editor_app_open_file(const char* path) {
    if (ed_is_open && ed_window) {
        gui_window_focus(ed_window);
        gui_dirty_add(ed_window->x, ed_window->y,
                      ed_window->width, ed_window->height);
        if (path && path[0]) ed_load_file(path);
        return;
    }

    ed_window = gui_window_create("Notes", 120, 50, ED_WIN_W, ED_WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED | GUI_WIN_RESIZABLE);
    if (!ed_window) return;

    ed_window->on_paint = ed_paint;
    ed_window->on_key = ed_on_key;
    ed_window->on_click = ed_on_click;
    ed_window->on_close = ed_on_close;

    ed_is_open = 1;
    ed_buf[0] = '\0';
    ed_buf_len = 0;
    ed_filepath[0] = '\0';
    ed_cur_line = 0;
    ed_cur_col = 0;
    ed_scroll_y = 0;
    ed_modified = 0;
    ed_status[0] = '\0';

    if (path && path[0]) {
        ed_load_file(path);
    } else {
        ed_set_status("New file — Ctrl+S to save", COL_ED_ACCENT);
    }
}
