/* ============================================================
 *  dialog.c  —  Common File Dialogs (Open / Save)
 *
 *  Reusable open/save file picker that apps can invoke.
 *  Shows a directory listing with navigation; the user picks
 *  a file or types a new filename (save mode).
 * ============================================================ */

#include "dialog.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "vfs.h"
#include "theme.h"

/* ---- Colours (from theme.h) ---- */
#define COL_DLG_BG      THEME_COL_BASE
#define COL_DLG_TEXT     THEME_COL_TEXT
#define COL_DLG_DIM      THEME_COL_DIM
#define COL_DLG_ACCENT   THEME_COL_ACCENT
#define COL_DLG_PANEL    THEME_COL_MANTLE
#define COL_DLG_BORDER   THEME_COL_BORDER
#define COL_DLG_SELECT   THEME_COL_SURFACE1
#define COL_DLG_DIR      THEME_COL_SUCCESS
#define COL_DLG_BTN_BG   THEME_COL_SURFACE0
#define COL_DLG_BTN_HL   THEME_COL_ACCENT

/* ---- Layout ---- */
#define DLG_W          440
#define DLG_H          380
#define DLG_PATH_H     24
#define DLG_ROW_H      18
#define DLG_BTN_H      28
#define DLG_BTN_W      80
#define DLG_INPUT_H    24
#define DLG_PAD        8
#define DLG_LIST_TOP   (GUI_TITLEBAR_HEIGHT + DLG_PATH_H + 4)
#define DLG_MAX_ENTRIES 128
#define DLG_NAME_MAX   128

/* ---- State ---- */
typedef struct {
    char name[DLG_NAME_MAX];
    int is_dir;
} dlg_entry_t;

typedef struct {
    gui_window_t* win;
    int mode;           /* 0 = open, 1 = save */
    char cwd[256];
    dlg_entry_t entries[DLG_MAX_ENTRIES];
    int entry_count;
    int selected;
    int scroll;

    /* Save mode: filename input */
    char input_buf[DLG_NAME_MAX];
    int input_len;
    int input_active;

    dialog_callback_t cb;
    void* userdata;
} dialog_state_t;

static dialog_state_t dst;
static int dlg_open;

/* ---- Build sorted file list ---- */
static void dlg_refresh(void) {
    int fd, idx = 0;
    char name[DLG_NAME_MAX];
    int i, j;

    dst.entry_count = 0;
    dst.selected = 0;
    dst.scroll = 0;

    fd = vfs_open(dst.cwd);
    if (fd < 0) return;

    /* Read entries */
    while (idx < DLG_MAX_ENTRIES) {
        int r = vfs_readdir(fd, (unsigned int)idx, name, sizeof(name));
        if (r != 0) break;
        if (str_compare(name, ".") == 0) { idx++; continue; }

        str_copy(dst.entries[dst.entry_count].name, name, DLG_NAME_MAX);

        /* Determine if directory via vfs_stat */
        {
            char full[512];
            vfs_stat_t st;
            str_copy(full, dst.cwd, 512);
            if (str_length(full) > 1) str_append(full, "/", 512);
            str_append(full, name, 512);
            if (vfs_stat(full, &st) == 0)
                dst.entries[dst.entry_count].is_dir = (st.flags & VFS_FLAG_DIR) ? 1 : 0;
            else
                dst.entries[dst.entry_count].is_dir = 0;
        }

        dst.entry_count++;
        idx++;
    }
    vfs_close(fd);

    /* Sort: dirs first, then alphabetical */
    for (i = 0; i < dst.entry_count - 1; i++) {
        for (j = i + 1; j < dst.entry_count; j++) {
            int swap = 0;
            if (dst.entries[i].is_dir != dst.entries[j].is_dir) {
                if (!dst.entries[i].is_dir && dst.entries[j].is_dir) swap = 1;
            } else {
                if (str_compare(dst.entries[i].name, dst.entries[j].name) > 0) swap = 1;
            }
            if (swap) {
                dlg_entry_t tmp = dst.entries[i];
                dst.entries[i] = dst.entries[j];
                dst.entries[j] = tmp;
            }
        }
    }
}

/* ---- Navigate into directory ---- */
static void dlg_navigate(const char* dir) {
    if (str_compare(dir, "..") == 0) {
        /* Go up */
        int len = str_length(dst.cwd);
        if (len <= 1) return;
        len--;
        while (len > 0 && dst.cwd[len] != '/') len--;
        if (len == 0) len = 1;
        dst.cwd[len] = '\0';
    } else {
        int cwdlen = str_length(dst.cwd);
        if (cwdlen > 1) str_append(dst.cwd, "/", 256);
        str_append(dst.cwd, dir, 256);
    }
    dlg_refresh();
}

/* ---- Confirm selection and close ---- */
static void dlg_confirm(void) {
    char full_path[512];
    str_copy(full_path, dst.cwd, 512);

    if (dst.mode == 1 && dst.input_len > 0) {
        /* Save mode: use typed filename */
        if (str_length(full_path) > 1) str_append(full_path, "/", 512);
        dst.input_buf[dst.input_len] = '\0';
        str_append(full_path, dst.input_buf, 512);
    } else if (dst.selected >= 0 && dst.selected < dst.entry_count) {
        dlg_entry_t* e = &dst.entries[dst.selected];
        if (e->is_dir) {
            dlg_navigate(e->name);
            return;
        }
        if (str_length(full_path) > 1) str_append(full_path, "/", 512);
        str_append(full_path, e->name, 512);
    } else {
        return;
    }

    if (dst.cb) dst.cb(full_path, dst.userdata);

    /* Close dialog */
    if (dst.win) {
        gui_dirty_add(dst.win->x, dst.win->y, dst.win->width, dst.win->height);
        gui_window_destroy(dst.win);
        dst.win = 0;
    }
    dlg_open = 0;
}

static void dlg_cancel(void) {
    if (dst.cb) dst.cb(0, dst.userdata);
    if (dst.win) {
        gui_dirty_add(dst.win->x, dst.win->y, dst.win->width, dst.win->height);
        gui_window_destroy(dst.win);
        dst.win = 0;
    }
    dlg_open = 0;
}

/* ---- Paint ---- */
static void dlg_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cx = GUI_BORDER_WIDTH;
    int cy = GUI_TITLEBAR_HEIGHT;
    int cw = win->width - 2 * GUI_BORDER_WIDTH;
    int list_bottom;
    int vis_rows;
    int i, y;

    if (!s->pixels) return;
    gui_surface_clear(s, COL_DLG_BG);

    /* Path bar */
    gui_surface_fill(s, cx, cy, cw, DLG_PATH_H, COL_DLG_PANEL);
    gui_surface_hline(s, cx, cy + DLG_PATH_H - 1, cw, COL_DLG_BORDER);
    gui_surface_draw_string(s, cx + DLG_PAD, cy + 4, dst.cwd, COL_DLG_ACCENT, 0, 0);

    /* File list */
    list_bottom = win->height - GUI_BORDER_WIDTH - DLG_BTN_H - 4;
    if (dst.mode == 1) list_bottom -= DLG_INPUT_H + 4;
    vis_rows = (list_bottom - DLG_LIST_TOP) / DLG_ROW_H;

    y = DLG_LIST_TOP;
    for (i = dst.scroll; i < dst.entry_count && (i - dst.scroll) < vis_rows; i++) {
        dlg_entry_t* e = &dst.entries[i];
        int ry = y + (i - dst.scroll) * DLG_ROW_H;

        if (i == dst.selected)
            gui_surface_fill(s, cx, ry, cw, DLG_ROW_H, COL_DLG_SELECT);

        /* Icon */
        gui_surface_draw_char(s, cx + DLG_PAD, ry + 1,
                              e->is_dir ? '/' : ' ',
                              e->is_dir ? COL_DLG_DIR : COL_DLG_DIM, 0, 0);

        /* Name */
        gui_surface_draw_string_n(s, cx + DLG_PAD + GUI_FONT_W + 4, ry + 1,
                                  e->name,
                                  (cw - DLG_PAD * 2 - GUI_FONT_W - 4) / GUI_FONT_W,
                                  e->is_dir ? COL_DLG_DIR : COL_DLG_TEXT, 0, 0);
    }

    /* Save mode: filename input */
    if (dst.mode == 1) {
        int iy = list_bottom + 2;
        gui_surface_fill(s, cx + DLG_PAD, iy, cw - DLG_PAD * 2, DLG_INPUT_H, COL_DLG_PANEL);
        gui_surface_hline(s, cx + DLG_PAD, iy, cw - DLG_PAD * 2,
                          dst.input_active ? COL_DLG_ACCENT : COL_DLG_BORDER);
        gui_surface_hline(s, cx + DLG_PAD, iy + DLG_INPUT_H - 1, cw - DLG_PAD * 2,
                          dst.input_active ? COL_DLG_ACCENT : COL_DLG_BORDER);

        if (dst.input_len > 0) {
            gui_surface_draw_string_n(s, cx + DLG_PAD + 4, iy + 4,
                                      dst.input_buf, dst.input_len,
                                      COL_DLG_TEXT, 0, 0);
        } else {
            gui_surface_draw_string(s, cx + DLG_PAD + 4, iy + 4,
                                    "filename...", COL_DLG_DIM, 0, 0);
        }
    }

    /* Buttons */
    {
        int by = win->height - GUI_BORDER_WIDTH - DLG_BTN_H - 2;
        int bx_ok = win->width - GUI_BORDER_WIDTH - DLG_PAD - DLG_BTN_W;
        int bx_cancel = bx_ok - DLG_BTN_W - 8;

        gui_surface_fill(s, bx_cancel, by, DLG_BTN_W, DLG_BTN_H, COL_DLG_BTN_BG);
        gui_surface_draw_string(s, bx_cancel + 16, by + 6, "Cancel", COL_DLG_DIM, 0, 0);

        gui_surface_fill(s, bx_ok, by, DLG_BTN_W, DLG_BTN_H, COL_DLG_BTN_HL);
        gui_surface_draw_string(s, bx_ok + 16, by + 6,
                                dst.mode == 0 ? "Open" : "Save",
                                COL_DLG_BG, 0, 0);
    }
}

/* ---- Key handling ---- */
static void dlg_key(gui_window_t* win, int event_type, char key) {
    if (event_type != 1) return;

    /* Save mode: if input active, type into input */
    if (dst.mode == 1 && dst.input_active) {
        if (key == '\n') {
            dlg_confirm();
            return;
        }
        if (key == '\b') {
            if (dst.input_len > 0) dst.input_len--;
        } else if (key == 0x1B) { /* Escape */
            dst.input_active = 0;
        } else if (key >= 0x20 && key < 0x7F && dst.input_len < DLG_NAME_MAX - 1) {
            dst.input_buf[dst.input_len++] = key;
        }
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
        return;
    }

    if (key == 0x1B) { /* Escape */
        dlg_cancel();
        return;
    }
    if (key == (char)0x48) { /* Up */
        if (dst.selected > 0) dst.selected--;
        /* Scroll if needed */
        if (dst.selected < dst.scroll) dst.scroll = dst.selected;
    } else if (key == (char)0x50) { /* Down */
        if (dst.selected < dst.entry_count - 1) dst.selected++;
        {
            int list_bottom = win->height - GUI_BORDER_WIDTH - DLG_BTN_H - 4;
            if (dst.mode == 1) list_bottom -= DLG_INPUT_H + 4;
            int vis = (list_bottom - DLG_LIST_TOP) / DLG_ROW_H;
            if (dst.selected >= dst.scroll + vis) dst.scroll = dst.selected - vis + 1;
        }
    } else if (key == '\n') {
        dlg_confirm();
        return;
    } else if (key == '\t' && dst.mode == 1) {
        dst.input_active = !dst.input_active;
    }

    win->needs_redraw = 1;
    gui_dirty_add(win->x, win->y, win->width, win->height);
}

/* ---- Click handling ---- */
static void dlg_click(gui_window_t* win, int x, int y, int button) {
    (void)button;
    int cx = GUI_BORDER_WIDTH;
    int cw = win->width - 2 * GUI_BORDER_WIDTH;

    /* Check button clicks */
    {
        int by = win->height - GUI_BORDER_WIDTH - DLG_BTN_H - 2;
        int bx_ok = win->width - GUI_BORDER_WIDTH - DLG_PAD - DLG_BTN_W;
        int bx_cancel = bx_ok - DLG_BTN_W - 8;

        if (y >= by && y < by + DLG_BTN_H) {
            if (x >= bx_ok && x < bx_ok + DLG_BTN_W) {
                dlg_confirm();
                return;
            }
            if (x >= bx_cancel && x < bx_cancel + DLG_BTN_W) {
                dlg_cancel();
                return;
            }
        }
    }

    /* Check save-mode input click */
    if (dst.mode == 1) {
        int list_bottom = win->height - GUI_BORDER_WIDTH - DLG_BTN_H - 4;
        list_bottom -= DLG_INPUT_H + 4;
        int iy = list_bottom + 2;
        if (y >= iy && y < iy + DLG_INPUT_H) {
            dst.input_active = 1;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
        dst.input_active = 0;
    }

    /* Check file list click */
    if (y >= DLG_LIST_TOP && x >= cx && x < cx + cw) {
        int row = (y - DLG_LIST_TOP) / DLG_ROW_H;
        int idx = dst.scroll + row;
        if (idx >= 0 && idx < dst.entry_count) {
            if (idx == dst.selected) {
                /* Double-click effect: navigate or confirm */
                dlg_confirm();
                return;
            }
            dst.selected = idx;

            /* For save mode, fill input with filename */
            if (dst.mode == 1 && !dst.entries[idx].is_dir) {
                str_copy(dst.input_buf, dst.entries[idx].name, DLG_NAME_MAX);
                dst.input_len = str_length(dst.input_buf);
            }
        }
    }

    win->needs_redraw = 1;
    gui_dirty_add(win->x, win->y, win->width, win->height);
}

/* ---- Close ---- */
static void dlg_close(gui_window_t* win) {
    dlg_cancel();
}

/* ---- Internal open ---- */
static void dlg_open_impl(int mode, const char* start_dir, const char* default_name,
                           dialog_callback_t cb, void* userdata) {
    if (dlg_open && dst.win) {
        gui_window_focus(dst.win);
        return;
    }

    str_copy(dst.cwd, start_dir ? start_dir : "/", 256);
    dst.mode = mode;
    dst.cb = cb;
    dst.userdata = userdata;
    dst.input_len = 0;
    dst.input_active = 0;

    if (default_name && mode == 1) {
        str_copy(dst.input_buf, default_name, DLG_NAME_MAX);
        dst.input_len = str_length(dst.input_buf);
    }

    dlg_refresh();

    dst.win = gui_window_create(mode == 0 ? "Open File" : "Save File",
                                 140, 80, DLG_W, DLG_H,
                                 GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE |
                                 GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!dst.win) return;

    dst.win->on_paint = dlg_paint;
    dst.win->on_key   = dlg_key;
    dst.win->on_click = dlg_click;
    dst.win->on_close = dlg_close;
    dlg_open = 1;

    dst.win->needs_redraw = 1;
    gui_dirty_add(dst.win->x, dst.win->y, dst.win->width, dst.win->height);
}

/* ---- Public API ---- */
void dialog_open_file(const char* start_dir, dialog_callback_t cb, void* userdata) {
    dlg_open_impl(0, start_dir, 0, cb, userdata);
}

void dialog_save_file(const char* start_dir, const char* default_name,
                      dialog_callback_t cb, void* userdata) {
    dlg_open_impl(1, start_dir, default_name, cb, userdata);
}
