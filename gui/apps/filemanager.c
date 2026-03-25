/* ============================================================
 *  filemanager.c  —  Lyth File Manager (Nexus)
 *
 *  Graphical file browser: navigate, create, rename, delete.
 *  Uses VFS for all operations.
 * ============================================================ */

#include "filemanager.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "vfs.h"
#include "timer.h"
#include "input.h"

/* ---- Colours (from theme.h) ---- */
#define COL_FM_BG        THEME_COL_BASE
#define COL_FM_PANEL     THEME_COL_MANTLE
#define COL_FM_TEXT       THEME_COL_TEXT
#define COL_FM_DIM        THEME_COL_DIM
#define COL_FM_ACCENT     THEME_COL_ACCENT
#define COL_FM_SELECT     THEME_COL_SURFACE0
#define COL_FM_DIR        THEME_COL_ACCENT
#define COL_FM_FILE       THEME_COL_TEXT
#define COL_FM_SIZE       THEME_COL_SUBTEXT0
#define COL_FM_TOOLBAR    THEME_COL_CRUST
#define COL_FM_PATHBAR    THEME_COL_MANTLE
#define COL_FM_BTN        THEME_COL_SURFACE1
#define COL_FM_BTN_TEXT   THEME_COL_TEXT
#define COL_FM_ERR        THEME_COL_ERROR
#define COL_FM_OK         THEME_COL_SUCCESS
#define COL_FM_ICON_DIR   THEME_COL_WARNING
#define COL_FM_ICON_FILE  THEME_COL_SUBTEXT1
#define COL_FM_BORDER     THEME_COL_BORDER

/* ---- Layout ---- */
#define FM_WIN_W      520
#define FM_WIN_H      440
#define FM_TOOLBAR_H  32
#define FM_PATHBAR_H  24
#define FM_HEADER_H   (FM_TOOLBAR_H + FM_PATHBAR_H)
#define FM_ROW_H      20
#define FM_PAD        8
#define FM_NAME_COL   32
#define FM_SIZE_COL   (FM_WIN_W - 90)
#define FM_MAX_ENTRIES 128
#define FM_NAME_MAX   64
#define FM_PATH_MAX   256
#define FM_STATUS_H   20

/* ---- Entry ---- */
typedef struct {
    char name[FM_NAME_MAX];
    unsigned int size;
    int is_dir;
} fm_entry_t;

/* ---- State ---- */
static gui_window_t* fm_window;
static int fm_is_open;

static char fm_cwd[FM_PATH_MAX];
static fm_entry_t fm_entries[FM_MAX_ENTRIES];
static int fm_entry_count;
static int fm_selected;
static int fm_scroll;

static char fm_status[64];
static uint32_t fm_status_color;
static unsigned int fm_status_expire;

/* Rename / mkdir mode */
#define FM_MODE_NORMAL 0
#define FM_MODE_MKDIR  1
#define FM_MODE_RENAME 2
#define FM_MODE_DELETE_CONFIRM 3
static int fm_mode;
static char fm_input_buf[FM_NAME_MAX];
static int fm_input_len;

/* ---- Helpers ---- */

static int fm_visible_rows(void) {
    int content_h = FM_WIN_H - GUI_TITLEBAR_HEIGHT - FM_HEADER_H - FM_STATUS_H;
    return content_h / FM_ROW_H;
}

static void fm_set_status(const char* msg, uint32_t color) {
    int i = 0;
    while (msg[i] && i + 1 < (int)sizeof(fm_status)) {
        fm_status[i] = msg[i];
        i++;
    }
    fm_status[i] = '\0';
    fm_status_color = color;
    fm_status_expire = timer_get_uptime_ms() + 3000;
}

static void fm_build_path(char* out, int out_max, const char* dir, const char* name) {
    int i = 0;
    while (dir[i] && i + 1 < out_max) { out[i] = dir[i]; i++; }
    /* add separator if not root */
    if (i > 1 && out[i - 1] != '/' && i + 1 < out_max) out[i++] = '/';
    if (i == 1 && out[0] == '/') { /* root, don't double-slash */ }
    int j = 0;
    while (name[j] && i + 1 < out_max) { out[i++] = name[j++]; }
    out[i] = '\0';
}

/* ---- Directory scanning ---- */

static void fm_scan_dir(void) {
    int fd, idx;
    char name[128];
    vfs_stat_t st;
    char full[FM_PATH_MAX];

    fm_entry_count = 0;
    fm_selected = 0;
    fm_scroll = 0;

    fd = vfs_open(fm_cwd);
    if (fd < 0) {
        fm_set_status("Cannot open directory", COL_FM_ERR);
        return;
    }

    idx = 0;
    while (fm_entry_count < FM_MAX_ENTRIES &&
           vfs_readdir(fd, idx, name, sizeof(name)) == 0) {
        /* skip . and .. */
        if (name[0] == '.' && (name[1] == '\0' ||
            (name[1] == '.' && name[2] == '\0'))) {
            idx++;
            continue;
        }

        fm_entry_t* e = &fm_entries[fm_entry_count];
        str_copy(e->name, name, FM_NAME_MAX);

        fm_build_path(full, FM_PATH_MAX, fm_cwd, name);
        if (vfs_stat(full, &st) == 0) {
            e->is_dir = (st.flags & VFS_FLAG_DIR) ? 1 : 0;
            e->size = st.size;
        } else {
            e->is_dir = 0;
            e->size = 0;
        }

        fm_entry_count++;
        idx++;
    }

    vfs_close(fd);

    /* Sort: directories first, then alphabetical */
    {
        int i, j;
        for (i = 0; i < fm_entry_count - 1; i++) {
            for (j = i + 1; j < fm_entry_count; j++) {
                int swap = 0;
                if (fm_entries[i].is_dir && !fm_entries[j].is_dir) {
                    swap = 0; /* dir before file: correct */
                } else if (!fm_entries[i].is_dir && fm_entries[j].is_dir) {
                    swap = 1; /* file before dir: swap */
                } else {
                    /* same type: alphabetical */
                    swap = (str_compare(fm_entries[i].name, fm_entries[j].name) > 0);
                }
                if (swap) {
                    fm_entry_t tmp = fm_entries[i];
                    fm_entries[i] = fm_entries[j];
                    fm_entries[j] = tmp;
                }
            }
        }
    }
}

static void fm_navigate(const char* path) {
    str_copy(fm_cwd, path, FM_PATH_MAX);
    fm_scan_dir();
}

static void fm_go_up(void) {
    int len = str_length(fm_cwd);
    if (len <= 1) return; /* already at root */

    /* strip trailing slash */
    if (len > 1 && fm_cwd[len - 1] == '/') {
        fm_cwd[len - 1] = '\0';
        len--;
    }
    /* find last slash */
    int i = len - 1;
    while (i > 0 && fm_cwd[i] != '/') i--;
    if (i == 0) {
        fm_cwd[0] = '/';
        fm_cwd[1] = '\0';
    } else {
        fm_cwd[i] = '\0';
    }
    fm_scan_dir();
}

static void fm_enter_selected(void) {
    if (fm_selected < 0 || fm_selected >= fm_entry_count) return;
    fm_entry_t* e = &fm_entries[fm_selected];

    if (e->is_dir) {
        char path[FM_PATH_MAX];
        fm_build_path(path, FM_PATH_MAX, fm_cwd, e->name);
        fm_navigate(path);
    } else {
        /* For now, show file info in status */
        char msg[64];
        str_copy(msg, e->name, 40);
        str_append(msg, " (", sizeof(msg));
        /* format size */
        {
            char sz[16];
            uint_to_str(e->size, sz, sizeof(sz));
            str_append(msg, sz, sizeof(msg));
        }
        str_append(msg, " bytes)", sizeof(msg));
        fm_set_status(msg, COL_FM_ACCENT);
    }
}

/* ---- Actions ---- */

static void fm_do_delete(void) {
    if (fm_selected < 0 || fm_selected >= fm_entry_count) return;
    fm_entry_t* e = &fm_entries[fm_selected];
    char path[FM_PATH_MAX];
    fm_build_path(path, FM_PATH_MAX, fm_cwd, e->name);

    if (vfs_delete(path) == 0) {
        fm_set_status("Deleted", COL_FM_OK);
        fm_scan_dir();
    } else {
        fm_set_status("Cannot delete", COL_FM_ERR);
    }
}

static void fm_do_mkdir(void) {
    if (fm_input_len == 0) return;
    char path[FM_PATH_MAX];
    fm_build_path(path, FM_PATH_MAX, fm_cwd, fm_input_buf);

    if (vfs_create(path, VFS_FLAG_DIR) == 0) {
        fm_set_status("Folder created", COL_FM_OK);
        fm_scan_dir();
    } else {
        fm_set_status("Cannot create folder", COL_FM_ERR);
    }
}

static void fm_do_rename(void) {
    if (fm_input_len == 0) return;
    if (fm_selected < 0 || fm_selected >= fm_entry_count) return;
    fm_entry_t* e = &fm_entries[fm_selected];

    char old_path[FM_PATH_MAX];
    char new_path[FM_PATH_MAX];
    fm_build_path(old_path, FM_PATH_MAX, fm_cwd, e->name);
    fm_build_path(new_path, FM_PATH_MAX, fm_cwd, fm_input_buf);

    if (vfs_rename(old_path, new_path) == 0) {
        fm_set_status("Renamed", COL_FM_OK);
        fm_scan_dir();
    } else {
        fm_set_status("Cannot rename", COL_FM_ERR);
    }
}

/* Helper: format file size human-readable */
static void fm_format_size(unsigned int bytes, char* out, int out_max) {
    if (bytes < 1024) {
        uint_to_str(bytes, out, out_max);
        str_append(out, " B", out_max);
    } else if (bytes < 1024 * 1024) {
        uint_to_str(bytes / 1024, out, out_max);
        str_append(out, " KB", out_max);
    } else {
        uint_to_str(bytes / (1024 * 1024), out, out_max);
        str_append(out, " MB", out_max);
    }
}

/* ---- Drawing ---- */

static void fm_draw_icon(gui_surface_t* s, int x, int y, int is_dir) {
    if (is_dir) {
        /* Folder icon: small filled rectangle with tab */
        gui_surface_fill(s, x, y + 2, 6, 1, COL_FM_ICON_DIR);
        gui_surface_fill(s, x, y + 3, 12, 9, COL_FM_ICON_DIR);
        gui_surface_fill(s, x + 1, y + 4, 10, 7, 0x2B2B3B);
    } else {
        /* File icon: rectangle with folded corner */
        gui_surface_fill(s, x, y + 1, 10, 12, COL_FM_ICON_FILE);
        gui_surface_fill(s, x + 1, y + 2, 8, 10, 0x2B2B3B);
        gui_surface_fill(s, x + 6, y + 1, 4, 4, COL_FM_DIM);
    }
}

static void fm_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int w = win->width;
    int y;
    int vis_rows, i;

    if (!s->pixels) return;

    /* Background */
    gui_surface_clear(s, COL_FM_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    y = GUI_TITLEBAR_HEIGHT;

    /* Toolbar */
    gui_surface_fill(s, 0, y, w, FM_TOOLBAR_H, COL_FM_TOOLBAR);
    {
        int bx = FM_PAD;
        /* Back button */
        gui_surface_fill(s, bx, y + 4, 40, 22, COL_FM_BTN);
        gui_surface_draw_string(s, bx + 6, y + 7, "<- Up", COL_FM_BTN_TEXT, 0, 0);
        bx += 48;
        /* Refresh button */
        gui_surface_fill(s, bx, y + 4, 56, 22, COL_FM_BTN);
        gui_surface_draw_string(s, bx + 6, y + 7, "Reload", COL_FM_BTN_TEXT, 0, 0);
        bx += 64;
        /* New folder button */
        gui_surface_fill(s, bx, y + 4, 64, 22, COL_FM_BTN);
        gui_surface_draw_string(s, bx + 6, y + 7, "New Dir", COL_FM_BTN_TEXT, 0, 0);
        bx += 72;
        /* Delete button */
        gui_surface_fill(s, bx, y + 4, 48, 22, COL_FM_BTN);
        gui_surface_draw_string(s, bx + 6, y + 7, "Del", COL_FM_BTN_TEXT, 0, 0);
        bx += 56;
        /* Rename button */
        gui_surface_fill(s, bx, y + 4, 56, 22, COL_FM_BTN);
        gui_surface_draw_string(s, bx + 6, y + 7, "Rename", COL_FM_BTN_TEXT, 0, 0);
    }
    gui_surface_hline(s, 0, y + FM_TOOLBAR_H - 1, w, COL_FM_BORDER);
    y += FM_TOOLBAR_H;

    /* Path bar */
    gui_surface_fill(s, 0, y, w, FM_PATHBAR_H, COL_FM_PATHBAR);
    gui_surface_draw_string(s, FM_PAD, y + 4, fm_cwd, COL_FM_ACCENT, 0, 0);
    gui_surface_hline(s, 0, y + FM_PATHBAR_H - 1, w, COL_FM_BORDER);
    y += FM_PATHBAR_H;

    /* Column header */
    gui_surface_draw_string(s, FM_NAME_COL, y + 2, "Name", COL_FM_DIM, 0, 0);
    gui_surface_draw_string(s, FM_SIZE_COL, y + 2, "Size", COL_FM_DIM, 0, 0);
    y += FM_ROW_H;
    gui_surface_hline(s, 0, y - 1, w, COL_FM_BORDER);

    /* File list */
    vis_rows = fm_visible_rows() - 1; /* -1 for column header */
    for (i = 0; i < vis_rows && (fm_scroll + i) < fm_entry_count; i++) {
        int idx = fm_scroll + i;
        fm_entry_t* e = &fm_entries[idx];
        int ry = y + i * FM_ROW_H;

        /* Selection highlight */
        if (idx == fm_selected) {
            gui_surface_fill(s, 0, ry, w, FM_ROW_H, COL_FM_SELECT);
        }

        /* Icon */
        fm_draw_icon(s, FM_PAD, ry + 3, e->is_dir);

        /* Name */
        gui_surface_draw_string_n(s, FM_NAME_COL, ry + 2, e->name,
                                  (FM_SIZE_COL - FM_NAME_COL - 8) / GUI_FONT_W,
                                  e->is_dir ? COL_FM_DIR : COL_FM_FILE, 0, 0);

        /* Size (files only) */
        if (!e->is_dir) {
            char sz[16];
            fm_format_size(e->size, sz, sizeof(sz));
            gui_surface_draw_string(s, FM_SIZE_COL, ry + 2, sz, COL_FM_SIZE, 0, 0);
        } else {
            gui_surface_draw_string(s, FM_SIZE_COL, ry + 2, "<DIR>", COL_FM_DIM, 0, 0);
        }
    }

    /* If empty */
    if (fm_entry_count == 0) {
        gui_surface_draw_string(s, FM_PAD, y + 4, "(empty)", COL_FM_DIM, 0, 0);
    }

    /* Input bar (mkdir/rename mode) */
    if (fm_mode == FM_MODE_MKDIR || fm_mode == FM_MODE_RENAME) {
        int bar_y = FM_WIN_H - FM_STATUS_H - 24;
        const char* prompt = fm_mode == FM_MODE_MKDIR ? "New folder: " : "Rename to: ";
        gui_surface_fill(s, 0, bar_y, w, 24, COL_FM_PANEL);
        gui_surface_draw_string(s, FM_PAD, bar_y + 4, prompt, COL_FM_ACCENT, 0, 0);
        {
            int px = FM_PAD + str_length(prompt) * GUI_FONT_W;
            gui_surface_fill(s, px, bar_y + 2, 200, 18, COL_FM_BG);
            gui_surface_draw_string(s, px + 4, bar_y + 4, fm_input_buf, COL_FM_TEXT, 0, 0);
            /* cursor */
            gui_surface_fill(s, px + 4 + fm_input_len * GUI_FONT_W, bar_y + 4, 2, GUI_FONT_H, COL_FM_ACCENT);
        }
        gui_surface_hline(s, 0, bar_y - 1, w, COL_FM_BORDER);
    }

    /* Delete confirm */
    if (fm_mode == FM_MODE_DELETE_CONFIRM && fm_selected >= 0 && fm_selected < fm_entry_count) {
        int bar_y = FM_WIN_H - FM_STATUS_H - 24;
        gui_surface_fill(s, 0, bar_y, w, 24, COL_FM_PANEL);
        gui_surface_draw_string(s, FM_PAD, bar_y + 4, "Delete '", COL_FM_ERR, 0, 0);
        gui_surface_draw_string(s, FM_PAD + 8 * GUI_FONT_W, bar_y + 4,
                                fm_entries[fm_selected].name, COL_FM_TEXT, 0, 0);
        {
            int nx = FM_PAD + (8 + str_length(fm_entries[fm_selected].name)) * GUI_FONT_W;
            gui_surface_draw_string(s, nx, bar_y + 4, "'? [Y/N]", COL_FM_ERR, 0, 0);
        }
        gui_surface_hline(s, 0, bar_y - 1, w, COL_FM_BORDER);
    }

    /* Status bar */
    {
        int sy = FM_WIN_H - FM_STATUS_H;
        gui_surface_fill(s, 0, sy, w, FM_STATUS_H, COL_FM_TOOLBAR);
        gui_surface_hline(s, 0, sy, w, COL_FM_BORDER);

        if (fm_status[0] && timer_get_uptime_ms() < fm_status_expire) {
            gui_surface_draw_string(s, FM_PAD, sy + 2, fm_status, fm_status_color, 0, 0);
        } else {
            /* Show item count */
            char info[32];
            uint_to_str((unsigned int)fm_entry_count, info, sizeof(info));
            str_append(info, " items", sizeof(info));
            gui_surface_draw_string(s, FM_PAD, sy + 2, info, COL_FM_DIM, 0, 0);
        }
    }
}

/* ---- Key handling ---- */

static void fm_handle_input_key(int event_type, char key) {
    if (event_type == INPUT_EVENT_ENTER) {
        if (fm_mode == FM_MODE_MKDIR) fm_do_mkdir();
        else if (fm_mode == FM_MODE_RENAME) fm_do_rename();
        fm_mode = FM_MODE_NORMAL;
        fm_input_buf[0] = '\0';
        fm_input_len = 0;
    } else if (event_type == INPUT_EVENT_BACKSPACE) {
        if (fm_input_len > 0) fm_input_buf[--fm_input_len] = '\0';
    } else if (event_type == INPUT_EVENT_CHAR && key >= ' ' && key <= '~') {
        if (fm_input_len < FM_NAME_MAX - 1) {
            fm_input_buf[fm_input_len++] = key;
            fm_input_buf[fm_input_len] = '\0';
        }
    } else if (event_type == INPUT_EVENT_CHAR && key == 27) {
        /* Escape */
        fm_mode = FM_MODE_NORMAL;
        fm_input_buf[0] = '\0';
        fm_input_len = 0;
    }
}

static void fm_on_key(gui_window_t* win, int event_type, char key) {
    (void)win;
    int vis_rows = fm_visible_rows() - 1;

    /* Input mode */
    if (fm_mode == FM_MODE_MKDIR || fm_mode == FM_MODE_RENAME) {
        fm_handle_input_key(event_type, key);
        gui_window_invalidate(fm_window);
        return;
    }

    /* Delete confirm */
    if (fm_mode == FM_MODE_DELETE_CONFIRM) {
        if (event_type == INPUT_EVENT_CHAR && (key == 'y' || key == 'Y')) {
            fm_do_delete();
        } else {
            fm_set_status("Cancelled", COL_FM_DIM);
        }
        fm_mode = FM_MODE_NORMAL;
        gui_window_invalidate(fm_window);
        return;
    }

    /* Normal navigation */
    if (event_type == INPUT_EVENT_UP) {
        if (fm_selected > 0) fm_selected--;
        if (fm_selected < fm_scroll) fm_scroll = fm_selected;
    } else if (event_type == INPUT_EVENT_DOWN) {
        if (fm_selected < fm_entry_count - 1) fm_selected++;
        if (fm_selected >= fm_scroll + vis_rows) fm_scroll = fm_selected - vis_rows + 1;
    } else if (event_type == INPUT_EVENT_ENTER) {
        fm_enter_selected();
    } else if (event_type == INPUT_EVENT_BACKSPACE) {
        fm_go_up();
    } else if (event_type == INPUT_EVENT_HOME) {
        fm_selected = 0;
        fm_scroll = 0;
    } else if (event_type == INPUT_EVENT_END) {
        fm_selected = fm_entry_count - 1;
        if (fm_selected >= vis_rows) fm_scroll = fm_selected - vis_rows + 1;
    } else if (event_type == INPUT_EVENT_CHAR) {
        switch (key) {
        case 'n': case 'N':
            fm_mode = FM_MODE_MKDIR;
            fm_input_buf[0] = '\0';
            fm_input_len = 0;
            break;
        case 'd': case 'D':
            if (fm_entry_count > 0)
                fm_mode = FM_MODE_DELETE_CONFIRM;
            break;
        case 'r': case 'R':
            if (fm_entry_count > 0) {
                fm_mode = FM_MODE_RENAME;
                str_copy(fm_input_buf, fm_entries[fm_selected].name, FM_NAME_MAX);
                fm_input_len = str_length(fm_input_buf);
            }
            break;
        case 'f': case 'F':
            fm_scan_dir(); /* refresh */
            break;
        case '/':
            fm_navigate("/");
            break;
        case '~':
            fm_navigate("/home");
            break;
        }
    }

    gui_window_invalidate(fm_window);
}

/* ---- Mouse click ---- */

static void fm_on_click(gui_window_t* win, int mx, int my, int button) {
    (void)button;
    int y = GUI_TITLEBAR_HEIGHT;
    int vis_rows;

    /* Toolbar clicks */
    if (my >= y && my < y + FM_TOOLBAR_H) {
        int bx = FM_PAD;
        /* Up button */
        if (mx >= bx && mx < bx + 40) { fm_go_up(); }
        bx += 48;
        /* Refresh */
        if (mx >= bx && mx < bx + 56) { fm_scan_dir(); }
        bx += 64;
        /* New Dir */
        if (mx >= bx && mx < bx + 64) {
            fm_mode = FM_MODE_MKDIR;
            fm_input_buf[0] = '\0';
            fm_input_len = 0;
        }
        bx += 72;
        /* Delete */
        if (mx >= bx && mx < bx + 48) {
            if (fm_entry_count > 0)
                fm_mode = FM_MODE_DELETE_CONFIRM;
        }
        bx += 56;
        /* Rename */
        if (mx >= bx && mx < bx + 56) {
            if (fm_entry_count > 0) {
                fm_mode = FM_MODE_RENAME;
                str_copy(fm_input_buf, fm_entries[fm_selected].name, FM_NAME_MAX);
                fm_input_len = str_length(fm_input_buf);
            }
        }
        gui_window_invalidate(win);
        return;
    }
    y += FM_TOOLBAR_H + FM_PATHBAR_H + FM_ROW_H; /* skip header + col header */

    /* File list clicks */
    vis_rows = fm_visible_rows() - 1;
    if (my >= y) {
        int row = (my - y) / FM_ROW_H;
        int idx = fm_scroll + row;
        if (idx >= 0 && idx < fm_entry_count) {
            if (fm_selected == idx) {
                /* Double-click effect: enter */
                fm_enter_selected();
            } else {
                fm_selected = idx;
            }
        }
    }

    gui_window_invalidate(win);
}

/* ---- Close ---- */

static void fm_on_close(gui_window_t* win) {
    fm_is_open = 0;
    fm_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- Public API ---- */

void filemanager_app_open(void) {
    filemanager_app_open_at("/");
}

void filemanager_app_open_at(const char* path) {
    if (fm_is_open && fm_window) {
        gui_window_focus(fm_window);
        gui_dirty_add(fm_window->x, fm_window->y,
                      fm_window->width, fm_window->height);
        return;
    }

    fm_window = gui_window_create("Nexus", 80, 40, FM_WIN_W, FM_WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!fm_window) return;

    fm_window->on_paint = fm_paint;
    fm_window->on_key = fm_on_key;
    fm_window->on_click = fm_on_click;
    fm_window->on_close = fm_on_close;

    fm_is_open = 1;
    fm_mode = FM_MODE_NORMAL;
    fm_status[0] = '\0';

    str_copy(fm_cwd, path ? path : "/", FM_PATH_MAX);
    fm_scan_dir();
}
