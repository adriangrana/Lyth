/* ============================================================
 *  login.c  —  Graphical login manager
 *
 *  Displays a login screen using the GUI compositor.  The user
 *  selects or types a username, enters a password, and the
 *  manager validates credentials via ugdb.  On success the
 *  authenticated uid is returned so init can create a session.
 * ============================================================ */

#include "login.h"
#include "compositor.h"
#include "window.h"
#include "desktop.h"
#include "cursor.h"
#include "input.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "timer.h"
#include "ugdb.h"
#include "usb_hid.h"
#include "e1000.h"
#include "task.h"
#include "string.h"
#include "rtc.h"

/* ---- Colours ---- */
#define COL_LOGIN_BG       0x1E1E2E
#define COL_LOGIN_PANEL    0x313244
#define COL_LOGIN_BORDER   0x45475A
#define COL_LOGIN_TITLE    0xCDD6F4
#define COL_LOGIN_LABEL    0xA6ADC8
#define COL_LOGIN_INPUT_BG 0x1E1E2E
#define COL_LOGIN_INPUT_FG 0xCDD6F4
#define COL_LOGIN_BTN_BG   0x89B4FA
#define COL_LOGIN_BTN_FG   0x1E1E2E
#define COL_LOGIN_ERROR    0xF38BA8
#define COL_LOGIN_CURSOR   0xF5C2E7

/* ---- Layout ---- */
#define PANEL_W     340
#define PANEL_H     280
#define FIELD_W     260
#define FIELD_H     28
#define BTN_W       260
#define BTN_H       32
#define FIELD_PAD   8
#define MAX_INPUT   15

/* ---- State ---- */
static int login_active;
static int login_done;
static unsigned int login_result_uid;

static char username_buf[MAX_INPUT + 1];
static int  username_len;
static char password_buf[MAX_INPUT + 1];
static int  password_len;
static int  focus_field;  /* 0 = username, 1 = password */
static char error_msg[48];
static int  show_error;

/* Screen dimensions (set by login_manager_run) */
static int scr_w, scr_h;

/* ---- Drawing helpers (render into backbuffer) ---- */

static void login_fill_rect(gui_surface_t* s, int x, int y, int w, int h, uint32_t c) {
    gui_surface_fill(s, x, y, w, h, c);
}

static void login_draw_string(gui_surface_t* s, int x, int y,
                               const char* str, uint32_t fg) {
    gui_surface_draw_string(s, x, y, str, fg, 0, 0);
}

static int str_pixel_width(const char* s) {
    int len = 0;
    while (s[len]) len++;
    return len * GUI_FONT_W;
}

/* ---- Render the login screen into the compositor backbuffer ---- */

static void login_paint(gui_surface_t* bb) {
    int px, py;     /* panel top-left */
    int fy;         /* current field y */
    int i;
    rtc_time_t rt;
    char clock[9];

    /* full-screen background */
    gui_surface_clear(bb, COL_LOGIN_BG);

    /* centre panel */
    px = (scr_w - PANEL_W) / 2;
    py = (scr_h - PANEL_H) / 2;

    /* panel background */
    login_fill_rect(bb, px, py, PANEL_W, PANEL_H, COL_LOGIN_PANEL);

    /* border */
    gui_surface_hline(bb, px, py, PANEL_W, COL_LOGIN_BORDER);
    gui_surface_hline(bb, px, py + PANEL_H - 1, PANEL_W, COL_LOGIN_BORDER);
    {
        int r;
        for (r = py; r < py + PANEL_H; r++) {
            gui_surface_putpixel(bb, px, r, COL_LOGIN_BORDER);
            gui_surface_putpixel(bb, px + PANEL_W - 1, r, COL_LOGIN_BORDER);
        }
    }

    /* title */
    {
        const char* title = "Lyth OS";
        int tw = str_pixel_width(title);
        login_draw_string(bb, px + (PANEL_W - tw) / 2, py + 20, title, COL_LOGIN_TITLE);
    }

    /* Username label + field */
    fy = py + 60;
    {
        int fx = px + (PANEL_W - FIELD_W) / 2;
        login_draw_string(bb, fx, fy, "User", COL_LOGIN_LABEL);
        fy += GUI_FONT_H + 4;
        login_fill_rect(bb, fx, fy, FIELD_W, FIELD_H,
                        focus_field == 0 ? COL_LOGIN_BORDER : COL_LOGIN_INPUT_BG);
        login_fill_rect(bb, fx + 1, fy + 1, FIELD_W - 2, FIELD_H - 2, COL_LOGIN_INPUT_BG);
        login_draw_string(bb, fx + FIELD_PAD, fy + 6, username_buf, COL_LOGIN_INPUT_FG);

        /* cursor */
        if (focus_field == 0) {
            int cx = fx + FIELD_PAD + username_len * GUI_FONT_W;
            login_fill_rect(bb, cx, fy + 4, 2, FIELD_H - 8, COL_LOGIN_CURSOR);
        }
    }

    /* Password label + field */
    fy += FIELD_H + 12;
    {
        int fx = px + (PANEL_W - FIELD_W) / 2;
        login_draw_string(bb, fx, fy, "Password", COL_LOGIN_LABEL);
        fy += GUI_FONT_H + 4;
        login_fill_rect(bb, fx, fy, FIELD_W, FIELD_H,
                        focus_field == 1 ? COL_LOGIN_BORDER : COL_LOGIN_INPUT_BG);
        login_fill_rect(bb, fx + 1, fy + 1, FIELD_W - 2, FIELD_H - 2, COL_LOGIN_INPUT_BG);

        /* show dots instead of password chars */
        {
            char dots[MAX_INPUT + 1];
            for (i = 0; i < password_len && i < MAX_INPUT; i++) dots[i] = '*';
            dots[i] = '\0';
            login_draw_string(bb, fx + FIELD_PAD, fy + 6, dots, COL_LOGIN_INPUT_FG);
        }

        if (focus_field == 1) {
            int cx = fx + FIELD_PAD + password_len * GUI_FONT_W;
            login_fill_rect(bb, cx, fy + 4, 2, FIELD_H - 8, COL_LOGIN_CURSOR);
        }
    }

    /* Login button */
    fy += FIELD_H + 16;
    {
        int fx = px + (PANEL_W - BTN_W) / 2;
        const char* blabel = "Login";
        int bw = str_pixel_width(blabel);
        login_fill_rect(bb, fx, fy, BTN_W, BTN_H, COL_LOGIN_BTN_BG);
        login_draw_string(bb, fx + (BTN_W - bw) / 2, fy + 8, blabel, COL_LOGIN_BTN_FG);
    }

    /* Error message */
    if (show_error && error_msg[0]) {
        int ew = str_pixel_width(error_msg);
        login_draw_string(bb, px + (PANEL_W - ew) / 2, py + PANEL_H - 30,
                          error_msg, COL_LOGIN_ERROR);
    }

    /* Clock at top-right */
    rtc_read(&rt);
    {
        clock[0] = '0' + rt.hour / 10;
        clock[1] = '0' + rt.hour % 10;
        clock[2] = ':';
        clock[3] = '0' + rt.min / 10;
        clock[4] = '0' + rt.min % 10;
        clock[5] = ':';
        clock[6] = '0' + rt.sec / 10;
        clock[7] = '0' + rt.sec % 10;
        clock[8] = '\0';
    }
    login_draw_string(bb, scr_w - str_pixel_width(clock) - 12, 8, clock, COL_LOGIN_LABEL);
}

/* ---- Attempt login ---- */

static void try_login(void) {
    const ugdb_user_t* user;

    if (username_len == 0) {
        show_error = 1;
        memcpy(error_msg, "Enter a username", 17);
        return;
    }

    user = ugdb_find_by_name(username_buf);
    if (!user) {
        show_error = 1;
        memcpy(error_msg, "Unknown user", 13);
        return;
    }

    if (!ugdb_check_password(user->uid, password_buf)) {
        show_error = 1;
        memcpy(error_msg, "Invalid password", 17);
        return;
    }

    /* Success */
    login_result_uid = user->uid;
    login_done = 1;
}

/* ---- Input handling ---- */

static void login_handle_key(int event_type, char key) {
    /* Tab switches between fields */
    if (event_type == INPUT_EVENT_TAB) {
        focus_field = (focus_field + 1) % 2;
        show_error = 0;
        return;
    }

    /* Enter submits */
    if (event_type == INPUT_EVENT_ENTER) {
        try_login();
        return;
    }

    /* Backspace */
    if (event_type == INPUT_EVENT_BACKSPACE) {
        show_error = 0;
        if (focus_field == 0 && username_len > 0) {
            username_buf[--username_len] = '\0';
        } else if (focus_field == 1 && password_len > 0) {
            password_buf[--password_len] = '\0';
        }
        return;
    }

    /* Printable characters */
    if (event_type == INPUT_EVENT_CHAR && key >= ' ' && key <= '~') {
        show_error = 0;
        if (focus_field == 0 && username_len < MAX_INPUT) {
            username_buf[username_len++] = key;
            username_buf[username_len] = '\0';
        } else if (focus_field == 1 && password_len < MAX_INPUT) {
            password_buf[password_len++] = key;
            password_buf[password_len] = '\0';
        }
        return;
    }
}

static void login_handle_mouse_click(int mx, int my) {
    int px = (scr_w - PANEL_W) / 2;
    int py = (scr_h - PANEL_H) / 2;
    int fx = px + (PANEL_W - FIELD_W) / 2;
    int fy;

    /* Username field */
    fy = py + 60 + GUI_FONT_H + 4;
    if (mx >= fx && mx < fx + FIELD_W && my >= fy && my < fy + FIELD_H) {
        focus_field = 0;
        show_error = 0;
        return;
    }

    /* Password field */
    fy += FIELD_H + 12 + GUI_FONT_H + 4;
    if (mx >= fx && mx < fx + FIELD_W && my >= fy && my < fy + FIELD_H) {
        focus_field = 1;
        show_error = 0;
        return;
    }

    /* Login button */
    fy += FIELD_H + 16;
    if (mx >= fx && mx < fx + BTN_W && my >= fy && my < fy + BTN_H) {
        try_login();
        return;
    }
}

/* ---- Public API ---- */

unsigned int login_manager_run(void) {
    gui_surface_t* bb;
    input_event_t ev;
    int mouse_x, mouse_y;
    unsigned int last_sec = 0;

    scr_w = gui_screen_width();
    scr_h = gui_screen_height();
    bb = gui_get_backbuffer();
    if (!bb || !bb->pixels) return (unsigned int)-1;

    /* Reset state */
    login_active = 1;
    login_done = 0;
    login_result_uid = (unsigned int)-1;
    username_buf[0] = '\0'; username_len = 0;
    password_buf[0] = '\0'; password_len = 0;
    focus_field = 0;
    error_msg[0] = '\0';
    show_error = 0;
    mouse_x = scr_w / 2;
    mouse_y = scr_h / 2;

    cursor_init(scr_w, scr_h);
    cursor_set_pos(mouse_x, mouse_y);

    while (!login_done) {
        int need_repaint = 1;

        usb_hid_poll();
        e1000_poll_rx();

        /* Drain input */
        while (input_poll_event(&ev)) {
            if (ev.device_type == INPUT_DEVICE_KEYBOARD) {
                login_handle_key(ev.type, ev.character);
            }
            if (ev.device_type == INPUT_DEVICE_MOUSE) {
                mouse_x += ev.delta_x;
                mouse_y += ev.delta_y;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= scr_w) mouse_x = scr_w - 1;
                if (mouse_y >= scr_h) mouse_y = scr_h - 1;
                cursor_set_pos(mouse_x, mouse_y);

                if (ev.buttons & 0x01) {
                    login_handle_mouse_click(mouse_x, mouse_y);
                }
            }
        }

        /* Update clock every second */
        {
            unsigned int now_sec = timer_get_uptime_ms() / 1000;
            if (now_sec != last_sec) {
                last_sec = now_sec;
                need_repaint = 1;
            }
        }

        if (need_repaint) {
            login_paint(bb);
            cursor_draw(bb);

            /* Present full screen to framebuffer */
            {
                uint8_t* fb = (uint8_t*)fb_get_buffer();
                uint32_t pitch = fb_pitch();
                int row;
                if (fb && fb_bpp() == 32) {
                    for (row = 0; row < scr_h; row++) {
                        memcpy(fb + row * pitch,
                               &bb->pixels[row * bb->stride],
                               (size_t)scr_w * 4);
                    }
                }
            }
        }

        __asm__ volatile("hlt");
    }

    login_active = 0;

    /* Clear password from memory */
    memset(password_buf, 0, sizeof(password_buf));

    return login_result_uid;
}

void login_manager_request_logout(void) {
    /* This is called from compositor context.  It signals that
     * gui_run() should exit so init can loop back to login. */
    gui_stop();
}

int login_manager_is_active(void) {
    return login_active;
}
