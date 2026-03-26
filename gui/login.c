/* ============================================================
 *  login.c  —  Graphical login manager
 *
 *  Modern login / lock screen with sky background, large clock,
 *  user avatar, and password field.
 * ============================================================ */

#include "login.h"
#include "compositor.h"
#include "renderer.h"
#include "window.h"
#include "desktop.h"
#include "cursor.h"
#include "input.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "theme.h"
#include "timer.h"
#include "ugdb.h"
#include "usb_hid.h"
#include "e1000.h"
#include "task.h"
#include "string.h"
#include "rtc.h"

/* ---- Colours (from theme.h + login-specific overrides) ---- */
#define COL_BG_TOP       THEME_COL_WALL_TOP
#define COL_BG_MID       THEME_COL_WALL_MID
#define COL_BG_BOT       THEME_COL_WALL_BOT

#define COL_PANEL_BG     THEME_COL_TASKBAR_BG
#define COL_PANEL_BORDER THEME_COL_POPUP_BORDER
#define COL_TEXT_WHITE   0xFFFFFF
#define COL_TEXT_DIM     THEME_COL_TASKBAR_DIM
#define COL_INPUT_BG     THEME_COL_SURFACE0
#define COL_INPUT_FG     0xFFFFFF
#define COL_INPUT_BORDER THEME_COL_BORDER_DIM
#define COL_INPUT_FOCUS  THEME_COL_FOCUS
#define COL_BTN_BG       THEME_COL_FOCUS
#define COL_BTN_FG       0xFFFFFF
#define COL_ERROR        THEME_COL_ERROR
#define COL_CURSOR       THEME_COL_CURSOR
#define COL_AVATAR_BG    THEME_COL_FOCUS
#define COL_AVATAR_RING  THEME_COL_ACCENT_HOVER
#define COL_CLOCK_FG     0xFFFFFF
#define COL_DATE_FG      0xD0E0F0
#define COL_ICON_DIM     THEME_COL_TASKBAR_DIM

/* ---- Layout ---- */
#define FIELD_W     240
#define FIELD_H     32
#define BTN_W       240
#define BTN_H       36
#define FIELD_PAD   10
#define MAX_INPUT   15
#define AVATAR_R    28   /* avatar circle radius */

/* ---- State ---- */
static volatile int login_active;
static volatile int login_done;
static volatile unsigned int login_result_uid;

static char username_buf[MAX_INPUT + 1];
static int  username_len;
static char password_buf[MAX_INPUT + 1];
static int  password_len;
static int  focus_field;  /* 0 = username, 1 = password */
static char error_msg[48];
static int  show_error;

static int scr_w, scr_h;

/* ---- Drawing helpers ---- */

static void login_fill(gui_surface_t* s, int x, int y, int w, int h, uint32_t c) {
    gui_surface_fill(s, x, y, w, h, c);
}

static void login_str(gui_surface_t* s, int x, int y, const char* str, uint32_t fg) {
    gui_surface_draw_string(s, x, y, str, fg, 0, 0);
}

static int str_pw(const char* s) {
    int n = 0; while (s[n]) n++; return n * GUI_FONT_W;
}

/* Mix two RGB colours: 0=a, 256=b */
static uint32_t mix_rgb(uint32_t a, uint32_t b, int t) {
    int ra = (a >> 16) & 0xFF, ga = (a >> 8) & 0xFF, ba = a & 0xFF;
    int rb = (b >> 16) & 0xFF, gb = (b >> 8) & 0xFF, bb = b & 0xFF;
    int r = ra + ((rb - ra) * t >> 8);
    int g = ga + ((gb - ga) * t >> 8);
    int bv = ba + ((bb - ba) * t >> 8);
    return ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (bv & 0xFF);
}

/* Draw a single character scaled by `scale`x */
static void draw_char_scaled(gui_surface_t* s, int x, int y,
                              unsigned char ch, uint32_t fg, int scale) {
    int row, col;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        uint8_t bits = font_psf_data[ch][row];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            if (bits & (0x80 >> col)) {
                gui_surface_fill(s, x + col * scale, y + row * scale,
                                 scale, scale, fg);
            }
        }
    }
}

/* Draw a string at `scale`x magnification */
static void draw_string_scaled(gui_surface_t* s, int x, int y,
                                const char* str, uint32_t fg, int scale) {
    while (*str) {
        draw_char_scaled(s, x, y, (unsigned char)*str, fg, scale);
        x += FONT_PSF_WIDTH * scale;
        str++;
    }
}

static int str_pw_scaled(const char* s, int scale) {
    int n = 0; while (s[n]) n++; return n * FONT_PSF_WIDTH * scale;
}

/* Draw filled circle (for avatar) */
static void draw_circle(gui_surface_t* s, int cx, int cy, int r, uint32_t c) {
    int y, x;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                gui_surface_putpixel(s, cx + x, cy + y, c);
            }
        }
    }
}

/* ---- Layout helpers (computed relative to screen centre) ---- */

/* Centre-X of entire screen */
#define CX (scr_w / 2)

/* Vertical positions (relative to screen) */
static int avatar_cy(void)   { return scr_h / 2 - 60; }
static int uname_y(void)     { return avatar_cy() + AVATAR_R + 12; }
static int field_y(int idx)  { return uname_y() + 24 + idx * (FIELD_H + 10); }
static int btn_y(void)       { return field_y(2); }
static int error_y(void)     { return btn_y() + BTN_H + 8; }
static int field_x(void)     { return CX - FIELD_W / 2; }

/* ---- Render the login screen ---- */

static void login_paint(gui_surface_t* bb, int full_bg) {
    int i;
    rtc_time_t rt;
    char clock_str[6];  /* HH:MM */
    char date_str[32];
    int fx, fy;

    /* ---- Full-screen sky gradient background ---- */
    if (full_bg) {
        int y;
        for (y = 0; y < scr_h; y++) {
            int t;
            uint32_t c;
            if (y < scr_h / 2) {
                t = y * 256 / (scr_h / 2);
                c = mix_rgb(COL_BG_TOP, COL_BG_MID, t);
            } else {
                t = (y - scr_h / 2) * 256 / (scr_h / 2);
                c = mix_rgb(COL_BG_MID, COL_BG_BOT, t);
            }
            gui_surface_hline(bb, 0, y, scr_w, c);
        }
    }

    /* ---- Read time ---- */
    rtc_read(&rt);
    clock_str[0] = '0' + rt.hour / 10;
    clock_str[1] = '0' + rt.hour % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + rt.min / 10;
    clock_str[4] = '0' + rt.min % 10;
    clock_str[5] = '\0';

    /* Day-of-week names */
    {
        static const char* dow_names[] = {
            "Sunday", "Monday", "Tuesday", "Wednesday",
            "Thursday", "Friday", "Saturday"
        };
        static const char* month_names[] = {
            "January","February","March","April","May","June",
            "July","August","September","October","November","December"
        };
        /* Compute day-of-week (Tomohiko Sakamoto's algorithm) */
        int dow;
        {
            static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
            int yr = (int)rt.year;
            int mn = (int)rt.month;
            int dy = (int)rt.day;
            if (mn < 3) yr--;
            dow = (yr + yr/4 - yr/100 + yr/400 + t[mn-1] + dy) % 7;
        }
        {
            const char* dowstr = dow_names[dow % 7];
            const char* mon = month_names[(rt.month > 0 && rt.month <= 12) ? rt.month - 1 : 0];
            /* Build "Wednesday, October 20" */
            int p = 0;
            const char* s;
            for (s = dowstr; *s && p < 28; s++) date_str[p++] = *s;
            date_str[p++] = ','; date_str[p++] = ' ';
            for (s = mon; *s && p < 28; s++) date_str[p++] = *s;
            date_str[p++] = ' ';
            if (rt.day >= 10) date_str[p++] = '0' + rt.day / 10;
            date_str[p++] = '0' + rt.day % 10;
            date_str[p] = '\0';
        }
    }

    /* ---- Large clock (3x scale) centred near top ---- */
    {
        int clock_scale = 3;
        int cw = str_pw_scaled(clock_str, clock_scale);
        int cx = CX - cw / 2;
        int cy = scr_h / 2 - 180;

        /* Clear clock area (for incremental repaints) */
        if (!full_bg) {
            login_fill(bb, cx - 4, cy - 2,
                       cw + 8, FONT_PSF_HEIGHT * clock_scale + 4,
                       mix_rgb(COL_BG_TOP, COL_BG_MID, 128));
        }
        draw_string_scaled(bb, cx, cy, clock_str, COL_CLOCK_FG, clock_scale);

        /* Date below clock (1x) */
        {
            int dw = str_pw(date_str);
            int dx = CX - dw / 2;
            int dy = cy + FONT_PSF_HEIGHT * clock_scale + 6;
            if (!full_bg) {
                login_fill(bb, dx - 4, dy - 2, dw + 8, GUI_FONT_H + 4,
                           mix_rgb(COL_BG_TOP, COL_BG_MID, 128));
            }
            login_str(bb, dx, dy, date_str, COL_DATE_FG);
        }
    }

    /* ---- Avatar circle ---- */
    {
        int acx = CX;
        int acy = avatar_cy();
        draw_circle(bb, acx, acy, AVATAR_R, COL_AVATAR_BG);
        draw_circle(bb, acx, acy, AVATAR_R + 1, COL_AVATAR_RING);
        draw_circle(bb, acx, acy, AVATAR_R - 1, COL_AVATAR_BG);

        /* User silhouette: head (small circle) + body (arc) */
        /* Head */
        draw_circle(bb, acx, acy - 8, 7, COL_TEXT_WHITE);
        /* Shoulders - small filled region */
        {
            int sy;
            for (sy = 3; sy <= 14; sy++) {
                int half = sy * 11 / 14;
                if (half > 14) half = 14;
                gui_surface_hline(bb, acx - half, acy + sy, half * 2, COL_TEXT_WHITE);
            }
        }
    }

    /* ---- Username label below avatar ---- */
    {
        const char* ustr = username_len > 0 ? username_buf : "User";
        int uw = str_pw(ustr);
        int ux = CX - uw / 2;
        int uy = uname_y();
        if (!full_bg) {
            login_fill(bb, CX - 80, uy - 2, 160, GUI_FONT_H + 4,
                       mix_rgb(COL_BG_MID, COL_BG_BOT, 64));
        }
        login_str(bb, ux, uy, ustr, COL_TEXT_WHITE);
    }

    /* ---- Username field ---- */
    fx = field_x();
    fy = field_y(0);
    {
        uint32_t brd = (focus_field == 0) ? COL_INPUT_FOCUS : COL_INPUT_BORDER;
        login_fill(bb, fx, fy, FIELD_W, FIELD_H, brd);
        login_fill(bb, fx + 1, fy + 1, FIELD_W - 2, FIELD_H - 2, COL_INPUT_BG);

        /* Icon placeholder: person symbol */
        login_str(bb, fx + 8, fy + 8, "@", COL_TEXT_DIM);

        /* Username text */
        if (username_len > 0) {
            login_str(bb, fx + 24, fy + 8, username_buf, COL_INPUT_FG);
        } else if (focus_field != 0) {
            login_str(bb, fx + 24, fy + 8, "Username", COL_TEXT_DIM);
        }

        if (focus_field == 0) {
            int cx = fx + 24 + username_len * GUI_FONT_W;
            login_fill(bb, cx, fy + 6, 2, FIELD_H - 12, COL_CURSOR);
        }
    }

    /* ---- Password field ---- */
    fy = field_y(1);
    {
        uint32_t brd = (focus_field == 1) ? COL_INPUT_FOCUS : COL_INPUT_BORDER;
        login_fill(bb, fx, fy, FIELD_W, FIELD_H, brd);
        login_fill(bb, fx + 1, fy + 1, FIELD_W - 2, FIELD_H - 2, COL_INPUT_BG);

        /* Lock icon placeholder */
        login_str(bb, fx + 8, fy + 8, "*", COL_TEXT_DIM);

        /* Password dots */
        if (password_len > 0) {
            char dots[MAX_INPUT + 1];
            for (i = 0; i < password_len && i < MAX_INPUT; i++) dots[i] = '*';
            dots[i] = '\0';
            login_str(bb, fx + 24, fy + 8, dots, COL_INPUT_FG);
        } else if (focus_field != 1) {
            login_str(bb, fx + 24, fy + 8, "Password", COL_TEXT_DIM);
        }

        if (focus_field == 1) {
            int cx = fx + 24 + password_len * GUI_FONT_W;
            login_fill(bb, cx, fy + 6, 2, FIELD_H - 12, COL_CURSOR);
        }
    }

    /* ---- Login button ---- */
    fy = btn_y();
    {
        const char* blabel = "Login  ->";
        int bw = str_pw(blabel);
        login_fill(bb, fx, fy, BTN_W, BTN_H, COL_BTN_BG);
        login_str(bb, fx + (BTN_W - bw) / 2, fy + 10, blabel, COL_BTN_FG);
    }

    /* ---- Error message ---- */
    if (show_error && error_msg[0]) {
        int ew = str_pw(error_msg);
        login_str(bb, CX - ew / 2, error_y(), error_msg, COL_ERROR);
    } else if (!full_bg) {
        /* Clear error area */
        login_fill(bb, CX - 120, error_y() - 2, 240, GUI_FONT_H + 4,
                   mix_rgb(COL_BG_MID, COL_BG_BOT, 128));
    }

    /* ---- Bottom bar: "Lyth OS" branding ---- */
    {
        const char* brand = "Lyth OS";
        int bw2 = str_pw(brand);
        login_str(bb, CX - bw2 / 2, scr_h - 30, brand, COL_ICON_DIM);
    }
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
    int fx = field_x();
    int fy;

    /* Username field */
    fy = field_y(0);
    if (mx >= fx && mx < fx + FIELD_W && my >= fy && my < fy + FIELD_H) {
        focus_field = 0;
        show_error = 0;
        return;
    }

    /* Password field */
    fy = field_y(1);
    if (mx >= fx && mx < fx + FIELD_W && my >= fy && my < fy + FIELD_H) {
        focus_field = 1;
        show_error = 0;
        return;
    }

    /* Login button */
    fy = btn_y();
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
    int initial_paint = 1;

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
        int need_repaint = 0;
        int cursor_moved = 0;
        int mouse_dx = 0, mouse_dy = 0;
        int mouse_btn = 0, had_mouse = 0;
        int prev_my = mouse_y;

        usb_hid_poll();
        e1000_poll_rx();

        /* Drain input, coalescing mouse moves */
        while (input_poll_event(&ev)) {
            if (ev.device_type == INPUT_DEVICE_KEYBOARD) {
                login_handle_key(ev.type, ev.character);
                need_repaint = 1;
            }
            if (ev.device_type == INPUT_DEVICE_MOUSE) {
                mouse_dx += ev.delta_x;
                mouse_dy += ev.delta_y;
                mouse_btn = ev.buttons;
                had_mouse = 1;
            }
        }

        /* Process coalesced mouse as a single update */
        if (had_mouse) {
            int old_mx = mouse_x, old_my = mouse_y;
            mouse_x += mouse_dx;
            mouse_y += mouse_dy;
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_x >= scr_w) mouse_x = scr_w - 1;
            if (mouse_y >= scr_h) mouse_y = scr_h - 1;

            if (mouse_x != old_mx || mouse_y != old_my)
                cursor_moved = 1;

            if (mouse_btn & 0x01) {
                login_handle_mouse_click(mouse_x, mouse_y);
                need_repaint = 1;
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

        if (initial_paint || need_repaint || cursor_moved) {
            /* Erase old cursor before modifying backbuffer */
            cursor_erase(bb);

            if (initial_paint || need_repaint) {
                login_paint(bb, initial_paint);
            }

            cursor_set_pos(mouse_x, mouse_y);
            cursor_draw(bb);

            /* Present through the renderer (works for both SW and virtio) */
            if (initial_paint) {
                gpu_present(0, 0, scr_w, scr_h);
                initial_paint = 0;
            } else {
                int y0, y1;

                if (need_repaint) {
                    y0 = 0;
                    y1 = scr_h;
                } else {
                    y0 = scr_h;
                    y1 = 0;
                }

                /* Expand for cursor bounding box (old + new) */
                {
                    int cy_lo = prev_my < mouse_y ? prev_my : mouse_y;
                    int cy_hi = prev_my > mouse_y ? prev_my : mouse_y;
                    if (cy_lo - 2 < y0) y0 = cy_lo - 2;
                    if (cy_hi + 20 > y1) y1 = cy_hi + 20;
                }
                if (y0 < 0) y0 = 0;
                if (y1 > scr_h) y1 = scr_h;

                if (y1 > y0)
                    gpu_present(0, y0, scr_w, y1 - y0);
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
