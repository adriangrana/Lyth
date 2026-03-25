/* ============================================================
 *  auth_prompt.c  —  Admin Authentication Prompt
 *
 *  A small modal-style window that asks the current session
 *  user to re-enter their password to authorise an action.
 * ============================================================ */

#include "auth_prompt.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "session.h"
#include "ugdb.h"

/* ---- Colours (Catppuccin Mocha) ---- */
#define COL_AP_BG      0x1E1E2E
#define COL_AP_TEXT     0xCDD6F4
#define COL_AP_DIM      0x6C7086
#define COL_AP_ACCENT   0x89B4FA
#define COL_AP_PANEL    0x181825
#define COL_AP_BORDER   0x313244
#define COL_AP_ERROR    0xF38BA8
#define COL_AP_BTN_OK   0x89B4FA
#define COL_AP_BTN_BG   0x313244

/* ---- Layout ---- */
#define AP_W           340
#define AP_H           200
#define AP_PAD         16
#define AP_FIELD_H     24
#define AP_BTN_W       80
#define AP_BTN_H       28
#define AP_MAX_PW      64

/* ---- State ---- */
static gui_window_t* ap_win;
static int ap_open;
static char ap_reason[128];
static char ap_pw_buf[AP_MAX_PW + 1];
static int  ap_pw_len;
static char ap_error[64];
static auth_callback_t ap_cb;
static void* ap_userdata;

/* ---- Internal ---- */
static void ap_close_grant(int granted) {
    auth_callback_t cb = ap_cb;
    void* ud = ap_userdata;

    if (ap_win) {
        gui_dirty_add(ap_win->x, ap_win->y, ap_win->width, ap_win->height);
        gui_window_destroy(ap_win);
        ap_win = 0;
    }
    ap_open = 0;
    ap_pw_len = 0;
    ap_pw_buf[0] = '\0';
    ap_error[0] = '\0';
    ap_cb = 0;

    if (cb) cb(granted, ud);
}

static void ap_attempt(void) {
    const session_t* sess = session_get_current();
    if (!sess) {
        str_copy(ap_error, "No active session", 64);
        return;
    }

    ap_pw_buf[ap_pw_len] = '\0';
    if (ugdb_check_password(sess->uid, ap_pw_buf)) {
        ap_close_grant(1);
        return;
    }

    str_copy(ap_error, "Incorrect password", 64);
    ap_pw_len = 0;
    ap_pw_buf[0] = '\0';

    if (ap_win) {
        ap_win->needs_redraw = 1;
        gui_dirty_add(ap_win->x, ap_win->y, ap_win->width, ap_win->height);
    }
}

/* ---- Paint ---- */
static void ap_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cx = GUI_BORDER_WIDTH + AP_PAD;
    int cy = GUI_TITLEBAR_HEIGHT + AP_PAD;
    int cw = win->width - 2 * (GUI_BORDER_WIDTH + AP_PAD);
    int y = cy;

    if (!s->pixels) return;
    gui_surface_clear(s, COL_AP_BG);

    /* Lock icon placeholder + title */
    gui_surface_draw_string(s, cx, y, "Authentication Required", COL_AP_ACCENT, 0, 0);
    y += GUI_FONT_H + 8;

    /* Reason */
    gui_surface_draw_string_n(s, cx, y, ap_reason, cw / GUI_FONT_W, COL_AP_DIM, 0, 0);
    y += GUI_FONT_H + 12;

    /* Password label */
    gui_surface_draw_string(s, cx, y, "Password:", COL_AP_TEXT, 0, 0);
    y += GUI_FONT_H + 4;

    /* Password field */
    gui_surface_fill(s, cx, y, cw, AP_FIELD_H, COL_AP_PANEL);
    gui_surface_hline(s, cx, y, cw, COL_AP_BORDER);
    gui_surface_hline(s, cx, y + AP_FIELD_H - 1, cw, COL_AP_BORDER);
    {
        /* Sides */
        int r;
        for (r = y; r < y + AP_FIELD_H; r++) {
            gui_surface_putpixel(s, cx, r, COL_AP_BORDER);
            gui_surface_putpixel(s, cx + cw - 1, r, COL_AP_BORDER);
        }
    }

    /* Password dots */
    {
        char dots[AP_MAX_PW + 1];
        int i;
        for (i = 0; i < ap_pw_len && i < AP_MAX_PW; i++) dots[i] = '*';
        dots[i] = '\0';
        gui_surface_draw_string(s, cx + 4, y + 4, dots, COL_AP_TEXT, 0, 0);

        /* Cursor */
        int cur_x = cx + 4 + ap_pw_len * GUI_FONT_W;
        gui_surface_fill(s, cur_x, y + 4, 2, GUI_FONT_H, COL_AP_ACCENT);
    }
    y += AP_FIELD_H + 4;

    /* Error message */
    if (ap_error[0]) {
        gui_surface_draw_string(s, cx, y, ap_error, COL_AP_ERROR, 0, 0);
    }

    /* Buttons */
    {
        int by = win->height - GUI_BORDER_WIDTH - AP_PAD - AP_BTN_H;
        int bx_ok = win->width - GUI_BORDER_WIDTH - AP_PAD - AP_BTN_W;
        int bx_cancel = bx_ok - AP_BTN_W - 8;

        gui_surface_fill(s, bx_cancel, by, AP_BTN_W, AP_BTN_H, COL_AP_BTN_BG);
        gui_surface_draw_string(s, bx_cancel + 16, by + 6, "Cancel", COL_AP_DIM, 0, 0);

        gui_surface_fill(s, bx_ok, by, AP_BTN_W, AP_BTN_H, COL_AP_BTN_OK);
        gui_surface_draw_string(s, bx_ok + 8, by + 6, "Confirm", COL_AP_BG, 0, 0);
    }
}

/* ---- Key ---- */
static void ap_key(gui_window_t* win, int event_type, char key) {
    if (event_type != 1) return;

    if (key == 0x1B) { /* Escape */
        ap_close_grant(0);
        return;
    }

    if (key == '\n') {
        ap_attempt();
        return;
    }

    if (key == '\b') {
        if (ap_pw_len > 0) ap_pw_len--;
    } else if (key >= 0x20 && key < 0x7F && ap_pw_len < AP_MAX_PW) {
        ap_pw_buf[ap_pw_len++] = key;
        ap_pw_buf[ap_pw_len] = '\0';
    }

    ap_error[0] = '\0';
    win->needs_redraw = 1;
    gui_dirty_add(win->x, win->y, win->width, win->height);
}

/* ---- Click ---- */
static void ap_click(gui_window_t* win, int x, int y, int button) {
    (void)button;

    int by = win->height - GUI_BORDER_WIDTH - AP_PAD - AP_BTN_H;
    int bx_ok = win->width - GUI_BORDER_WIDTH - AP_PAD - AP_BTN_W;
    int bx_cancel = bx_ok - AP_BTN_W - 8;

    if (y >= by && y < by + AP_BTN_H) {
        if (x >= bx_ok && x < bx_ok + AP_BTN_W) {
            ap_attempt();
            return;
        }
        if (x >= bx_cancel && x < bx_cancel + AP_BTN_W) {
            ap_close_grant(0);
            return;
        }
    }
}

/* ---- Close ---- */
static void ap_close(gui_window_t* win) {
    ap_close_grant(0);
}

/* ---- Public API ---- */
void auth_prompt_show(const char* reason, auth_callback_t cb, void* userdata) {
    if (ap_open && ap_win) {
        gui_window_focus(ap_win);
        return;
    }

    str_copy(ap_reason, reason ? reason : "An action requires authentication.", 128);
    ap_cb = cb;
    ap_userdata = userdata;
    ap_pw_len = 0;
    ap_pw_buf[0] = '\0';
    ap_error[0] = '\0';

    ap_win = gui_window_create("Authenticate", 200, 160, AP_W, AP_H,
                                GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE |
                                GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!ap_win) return;

    ap_win->on_paint = ap_paint;
    ap_win->on_key   = ap_key;
    ap_win->on_click = ap_click;
    ap_win->on_close = ap_close;
    ap_open = 1;

    ap_win->needs_redraw = 1;
    gui_dirty_add(ap_win->x, ap_win->y, ap_win->width, ap_win->height);
}
