/*
 * Desktop: wallpaper-style gradient background, modern dock-style taskbar,
 * app launcher overlay, notification/control center panels, and context menus.
 *
 * Inspired by modern desktop OS UIs with centered dock, system tray,
 * and frosted-glass styling (approximated with solid colours).
 */

#include "desktop.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "input.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"
#include "physmem.h"
#include "netif.h"
#include "login.h"
#include "acpi.h"

/* forward declarations for app launchers */
void terminal_app_open(void);
void taskman_app_open(void);
void sysinfo_app_open(void);
void netcfg_app_open(void);
void settings_app_open(void);
void filemanager_app_open(void);
void editor_app_open(void);
void calculator_app_open(void);
void about_app_open(void);
void viewer_app_open(void);

static void power_shutdown(void) { acpi_shutdown(); }
static void power_reboot(void) { acpi_reboot(); }

/* ---- colours: all sourced from theme.h ---- */
#define COL_BG_TOP       THEME_COL_WALL_TOP
#define COL_BG_MID       THEME_COL_WALL_MID
#define COL_BG_BOT       THEME_COL_WALL_BOT

#define COL_TASKBAR_BG   THEME_COL_TASKBAR_BG
#define COL_TASKBAR_SEP  THEME_COL_TASKBAR_SEP
#define COL_TASKBAR_TEXT THEME_COL_TASKBAR_TEXT
#define COL_TASKBAR_DIM  THEME_COL_TASKBAR_DIM

#define COL_DOCK_BG      THEME_COL_DOCK_BG
#define COL_DOCK_HOVER   THEME_COL_DOCK_HOVER
#define COL_DOCK_ACTIVE  THEME_COL_DOCK_ACTIVE
#define COL_DOCK_DOT     THEME_COL_DOCK_DOT

#define COL_TRAY_TEXT    THEME_COL_TASKBAR_TEXT
#define COL_TRAY_DIM     THEME_COL_TASKBAR_DIM

/* Launcher (app menu) colours */
#define COL_LAUNCH_BG    THEME_COL_LAUNCHER_BG
#define COL_LAUNCH_PANEL THEME_COL_LAUNCHER_PANEL
#define COL_LAUNCH_SEARCH_BG THEME_COL_LAUNCHER_SEARCH
#define COL_LAUNCH_TEXT  THEME_COL_LAUNCHER_TEXT
#define COL_LAUNCH_DIM   THEME_COL_LAUNCHER_DIM
#define COL_LAUNCH_HOVER THEME_COL_LAUNCHER_HOVER
#define COL_LAUNCH_SEP   THEME_COL_LAUNCHER_SEP
#define COL_LAUNCH_ICON_BG THEME_COL_LAUNCHER_ICON
#define COL_LAUNCH_FOLDER  THEME_COL_DOCK_BG

/* Context menu */
#define COL_CTX_BG       THEME_COL_POPUP_BG
#define COL_CTX_HOVER    THEME_COL_POPUP_HOVER
#define COL_CTX_TEXT     THEME_COL_POPUP_TEXT
#define COL_CTX_BORDER   THEME_COL_POPUP_BORDER

/* Network / control panel */
#define COL_POPUP_BG     THEME_COL_POPUP_BG
#define COL_POPUP_BORDER THEME_COL_POPUP_BORDER
#define COL_POPUP_TEXT   THEME_COL_POPUP_TEXT
#define COL_POPUP_DIM    THEME_COL_POPUP_DIM
#define COL_POPUP_BTN    THEME_COL_FOCUS

#define COL_NET_GREEN    THEME_COL_SUCCESS
#define COL_NET_RED      THEME_COL_ERROR

/* Running app label */
#define COL_APP_LABEL_BG THEME_COL_DOCK_BG
#define COL_APP_LABEL_FG THEME_COL_TEXT

/* ---- dock icon colours (per-app) ---- */
#define COL_ICON_FILES    THEME_COL_ICON_FILES
#define COL_ICON_SETTINGS THEME_COL_ICON_SETTINGS
#define COL_ICON_TERMINAL THEME_COL_ICON_TERMINAL
#define COL_ICON_CALC     THEME_COL_ICON_CALC
#define COL_ICON_EDITOR   THEME_COL_ICON_EDITOR
#define COL_ICON_VIEWER   THEME_COL_ICON_VIEWER
#define COL_ICON_TASKMAN  THEME_COL_ICON_TASKMAN
#define COL_ICON_NETCFG   THEME_COL_ICON_NETCFG
#define COL_ICON_ABOUT    THEME_COL_ICON_ABOUT
#define COL_ICON_SYSINFO  THEME_COL_ICON_SYSINFO

/* ---- dimensions ---- */
#define TASKBAR_H        GUI_TASKBAR_HEIGHT
#define DOCK_ICON_SIZE   28
#define DOCK_ICON_PAD    8
#define DOCK_MAX_ITEMS   12
#define DOCK_Y_PAD       4

/* ---- system tray dimensions ---- */
#define TRAY_ICON_W      22
#define TRAY_PAD         8
#define CLOCK_DATE_W     120

/* ---- dock item ---- */
typedef struct {
    const char* label;
    void (*action)(void);
    uint32_t icon_color;
    char shortcut;  /* first letter for icon */
} dock_item_t;

/* ---- start menu / app launcher ---- */
#define LAUNCHER_W      480
#define LAUNCHER_H      420
#define LAUNCHER_COLS   5
#define LAUNCHER_ICON_SZ 48
#define LAUNCHER_ICON_PAD 12
#define LAUNCHER_CELL_W  ((LAUNCHER_W - 40) / LAUNCHER_COLS)
#define LAUNCHER_CELL_H  72
#define LAUNCHER_MAX     20

typedef struct {
    char label[24];
    void (*action)(void);
    uint32_t icon_color;
    char icon_letter;
} launcher_item_t;

/* ---- context menu ---- */
#define CTX_W           140
#define CTX_ITEM_H      24
#define CTX_MAX_ITEMS   3

static int ctx_menu_open;
static int ctx_menu_x, ctx_menu_y;
static gui_window_t* ctx_target_win;

/* ---- network / control popup ---- */
#define NET_POPUP_W  240
#define NET_POPUP_H  180

/* ---- state ---- */
static int sw, sh;
static gui_surface_t desk_surf;
static int desk_valid;
static int start_menu_open;
static int menu_selected;

/* ---- dock items ---- */
static dock_item_t dock_items[DOCK_MAX_ITEMS];
static int dock_item_count;

/* ---- launcher items ---- */
static launcher_item_t launcher_items[LAUNCHER_MAX];
static int launcher_item_count;

/* ---- clock/date ---- */
static char clock_str[6]  = "00:00";
static char date_str[32]  = "";

/* ---- network popup ---- */
static int net_popup_open;
static int net_last_up;

/* ==================================================================*/

static int net_is_connected(void) {
    netif_t* iface = netif_get(0);
    return (iface && iface->up && iface->ip_addr != 0);
}

/* Draw a small dot (3x3 with center pixel + cross) */
static void draw_dot(gui_surface_t* dst, int cx, int cy, uint32_t col) {
    gui_surface_putpixel(dst, cx, cy, col);
    gui_surface_putpixel(dst, cx - 1, cy, col);
    gui_surface_putpixel(dst, cx + 1, cy, col);
    gui_surface_putpixel(dst, cx, cy - 1, col);
    gui_surface_putpixel(dst, cx, cy + 1, col);
}

/*
 * Clean rounded rect: fill + corner clip.
 * r=0: sharp, r=1: 1px corner cut, r=2: 2px smooth corner.
 * Works well at all sizes without pixelation.
 */
static void draw_rounded_rect(gui_surface_t* dst, int x, int y, int w, int h,
                               int r, uint32_t col) {
    int row;
    if (r < 1 || h < 4 || w < 4) {
        gui_surface_fill(dst, x, y, w, h, col);
        return;
    }
    if (r > 3) r = 3; /* cap to avoid complexity */
    /* Fill full rect then we are done — the corners are small enough
       that a simple inset approach is cleaner */
    if (r == 1) {
        /* top/bottom rows inset by 1px */
        gui_surface_fill(dst, x + 1, y, w - 2, 1, col);
        gui_surface_fill(dst, x, y + 1, w, h - 2, col);
        gui_surface_fill(dst, x + 1, y + h - 1, w - 2, 1, col);
    } else if (r == 2) {
        gui_surface_fill(dst, x + 2, y, w - 4, 1, col);
        gui_surface_fill(dst, x + 1, y + 1, w - 2, 1, col);
        gui_surface_fill(dst, x, y + 2, w, h - 4, col);
        gui_surface_fill(dst, x + 1, y + h - 2, w - 2, 1, col);
        gui_surface_fill(dst, x + 2, y + h - 1, w - 4, 1, col);
    } else { /* r == 3 */
        gui_surface_fill(dst, x + 3, y, w - 6, 1, col);
        gui_surface_fill(dst, x + 2, y + 1, w - 4, 1, col);
        gui_surface_fill(dst, x + 1, y + 2, w - 2, 1, col);
        gui_surface_fill(dst, x, y + 3, w, h - 6, col);
        gui_surface_fill(dst, x + 1, y + h - 3, w - 2, 1, col);
        gui_surface_fill(dst, x + 2, y + h - 2, w - 4, 1, col);
        gui_surface_fill(dst, x + 3, y + h - 1, w - 6, 1, col);
    }
}

/* tiny network/wifi icon (9x9) */
static void draw_net_icon(gui_surface_t* dst, int ox, int oy, int connected) {
    uint32_t col = connected ? COL_NET_GREEN : COL_NET_RED;
    int x, y;
    /* wifi-like arcs */
    for (x = 1; x < 8; x++) gui_surface_putpixel(dst, ox + x, oy, col);
    for (y = 1; y < 5; y++) {
        gui_surface_putpixel(dst, ox, oy + y, col);
        gui_surface_putpixel(dst, ox + 8, oy + y, col);
    }
    for (x = 1; x < 8; x++) gui_surface_putpixel(dst, ox + x, oy + 5, col);
    gui_surface_putpixel(dst, ox + 4, oy + 6, col);
    for (x = 2; x < 7; x++) gui_surface_putpixel(dst, ox + x, oy + 7, col);
    if (connected) {
        for (y = 2; y < 4; y++)
            for (x = 3; x < 6; x++)
                gui_surface_putpixel(dst, ox + x, oy + y, col);
    }
}

/* Draw a simple grid/menu icon (hamburger) */
static void draw_menu_icon(gui_surface_t* dst, int ox, int oy, uint32_t col) {
    gui_surface_fill(dst, ox, oy, 10, 2, col);
    gui_surface_fill(dst, ox, oy + 4, 10, 2, col);
    gui_surface_fill(dst, ox, oy + 8, 10, 2, col);
}

/* Draw volume icon */
static void draw_volume_icon(gui_surface_t* dst, int ox, int oy, uint32_t col) {
    gui_surface_fill(dst, ox, oy + 2, 3, 5, col);
    gui_surface_fill(dst, ox + 3, oy, 2, 9, col);
    gui_surface_putpixel(dst, ox + 6, oy + 2, col);
    gui_surface_putpixel(dst, ox + 7, oy + 1, col);
    gui_surface_putpixel(dst, ox + 6, oy + 6, col);
    gui_surface_putpixel(dst, ox + 7, oy + 7, col);
    gui_surface_putpixel(dst, ox + 6, oy + 4, col);
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, int t, int max) {
    if (max <= 0) max = 1;
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t rr = (ar * (uint32_t)(max - t) + br * (uint32_t)t) / (uint32_t)max;
    uint32_t rg = (ag * (uint32_t)(max - t) + bg * (uint32_t)t) / (uint32_t)max;
    uint32_t rb = (ab * (uint32_t)(max - t) + bb * (uint32_t)t) / (uint32_t)max;
    return (rr << 16) | (rg << 8) | rb;
}

static const char* day_names[] = {
    "Sunday", "Monday", "Tuesday", "Wednesday",
    "Thursday", "Friday", "Saturday"
};

static const char* month_names[] = {
    "", "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};

/* Zeller's formula for day-of-week (0=Sunday) */
static int day_of_week(int year, int month, int day) {
    int y = year, m = month;
    if (m < 3) { m += 12; y--; }
    return (day + (13 * (m + 1)) / 5 + y + y / 4 - y / 100 + y / 400) % 7;
}

static void update_clock(void) {
    rtc_time_t rtc;
    rtc_read(&rtc);
    clock_str[0] = '0' + (rtc.hour / 10) % 10;
    clock_str[1] = '0' + rtc.hour % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + (rtc.min / 10) % 10;
    clock_str[4] = '0' + rtc.min % 10;
    clock_str[5] = '\0';

    /* Build date string: "DayName, Month Day" */
    {
        int dow = day_of_week((int)rtc.year, (int)rtc.month, (int)rtc.day);
        const char* dn = day_names[dow % 7];
        const char* mn = (rtc.month >= 1 && rtc.month <= 12)
                         ? month_names[rtc.month] : "???";
        int pos = 0;
        while (*dn && pos < 28) date_str[pos++] = *dn++;
        if (pos < 28) date_str[pos++] = ',';
        if (pos < 28) date_str[pos++] = ' ';
        while (*mn && pos < 28) date_str[pos++] = *mn++;
        if (pos < 28) date_str[pos++] = ' ';
        if (rtc.day >= 10 && pos < 28) date_str[pos++] = '0' + (char)(rtc.day / 10);
        if (pos < 28) date_str[pos++] = '0' + (char)(rtc.day % 10);
        date_str[pos] = '\0';
    }
}

/* ---- Get dock metrics ---- */
static int dock_total_w(void) {
    return dock_item_count * (DOCK_ICON_SIZE + DOCK_ICON_PAD) + DOCK_ICON_PAD;
}
static int dock_start_x(void) {
    return (sw - dock_total_w()) / 2;
}

static void rebuild_desktop(void) {
    int y, taskbar_y;
    int half = sh / 2;

    if (!desk_surf.pixels) return;

    /* ---- Sky-gradient wallpaper ---- */
    for (y = 0; y < half; y++) {
        uint32_t c = mix_rgb(COL_BG_TOP, COL_BG_MID, y, half > 1 ? half - 1 : 1);
        memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
    }
    for (y = half; y < sh - TASKBAR_H; y++) {
        uint32_t c = mix_rgb(COL_BG_MID, COL_BG_BOT, y - half,
                             (sh - TASKBAR_H - half) > 1 ? (sh - TASKBAR_H - half - 1) : 1);
        memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
    }

    /* ---- Taskbar ---- */
    taskbar_y = sh - TASKBAR_H;

    /* Taskbar background with rounded top appearance */
    gui_surface_fill(&desk_surf, 0, taskbar_y, sw, TASKBAR_H, COL_TASKBAR_BG);
    gui_surface_hline(&desk_surf, 0, taskbar_y, sw, COL_TASKBAR_SEP);

    /* == Left section: running app labels == */
    {
        int lx = 8;
        int i, count = gui_window_count();
        for (i = 0; i < count; i++) {
            gui_window_t* w = gui_window_get(i);
            if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
            if (w->flags & GUI_WIN_MINIMIZED) continue;
            int tlen = (int)strlen(w->title);
            if (tlen > 12) tlen = 12;
            int item_w = tlen * FONT_PSF_WIDTH + 16;
            if (lx + item_w > dock_start_x() - 20) break;

            draw_rounded_rect(&desk_surf, lx, taskbar_y + 5, item_w,
                              TASKBAR_H - 10, 2, COL_APP_LABEL_BG);
            gui_surface_draw_string_n(&desk_surf, lx + 8,
                taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
                w->title, tlen, COL_APP_LABEL_FG, 0, 0);
            lx += item_w + 6;
        }
    }

    /* == Center: dock icons == */
    {
        int dx = dock_start_x();
        int dy = taskbar_y + DOCK_Y_PAD;
        int icon_h = TASKBAR_H - DOCK_Y_PAD * 2;
        int i;

        /* Dock background pill */
        draw_rounded_rect(&desk_surf, dx - 6, taskbar_y + 2,
                          dock_total_w() + 12, TASKBAR_H - 4, 3, COL_DOCK_BG);

        for (i = 0; i < dock_item_count; i++) {
            int ix = dx + DOCK_ICON_PAD + i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
            int iy = dy + (icon_h - DOCK_ICON_SIZE) / 2;

            /* Icon: colored rounded square with letter */
            draw_rounded_rect(&desk_surf, ix, iy, DOCK_ICON_SIZE,
                              DOCK_ICON_SIZE, 3, dock_items[i].icon_color);

            /* Draw shortcut letter in center */
            gui_surface_draw_char(&desk_surf,
                ix + (DOCK_ICON_SIZE - FONT_PSF_WIDTH) / 2,
                iy + (DOCK_ICON_SIZE - FONT_PSF_HEIGHT) / 2,
                (unsigned char)dock_items[i].shortcut,
                0xFFFFFF, 0, 0);

            /* Running indicator dot: check if any window title starts with dock label */
            {
                int j, wcount = gui_window_count();
                for (j = 0; j < wcount; j++) {
                    gui_window_t* w = gui_window_get(j);
                    if (w && (w->flags & GUI_WIN_VISIBLE) &&
                        str_starts_with(w->title, dock_items[i].label)) {
                        /* small dot below icon */
                        draw_dot(&desk_surf,
                            ix + DOCK_ICON_SIZE / 2,
                            taskbar_y + TASKBAR_H - 5,
                            COL_DOCK_DOT);
                        break;
                    }
                }
            }
        }
    }

    /* == Right section: tray icons + clock/date == */
    {
        int rx = sw - 12;

        /* Clock & date (rightmost) */
        {
            int time_w = 5 * FONT_PSF_WIDTH; /* "HH:MM" */
            int date_w = (int)strlen(date_str) * FONT_PSF_WIDTH;
            int block_w = (time_w > date_w ? time_w : date_w) + 8;
            rx -= block_w;

            /* Time (larger, top of tray area) */
            gui_surface_draw_string(&desk_surf, rx + 4,
                taskbar_y + 4,
                clock_str, COL_TRAY_TEXT, 0, 0);

            /* Date (smaller, below time) */
            gui_surface_draw_string(&desk_surf, rx + 4,
                taskbar_y + 4 + FONT_PSF_HEIGHT + 1,
                date_str, COL_TRAY_DIM, 0, 0);
            rx -= TRAY_PAD;
        }

        /* Volume icon */
        rx -= 12;
        draw_volume_icon(&desk_surf, rx, taskbar_y + (TASKBAR_H - 9) / 2,
                         COL_TRAY_TEXT);
        rx -= TRAY_PAD;

        /* Network icon */
        {
            int connected = net_is_connected();
            net_last_up = connected;
            rx -= 12;
            draw_net_icon(&desk_surf, rx, taskbar_y + (TASKBAR_H - 9) / 2,
                          connected);
            rx -= TRAY_PAD;
        }

        /* Menu/hamburger icon */
        rx -= 12;
        draw_menu_icon(&desk_surf, rx, taskbar_y + (TASKBAR_H - 10) / 2,
                       COL_TRAY_TEXT);
    }

    desk_valid = 1;
}

static void draw_start_menu(gui_surface_t* dst) {
    /* Modern app launcher overlay centred above taskbar */
    int lx = (sw - LAUNCHER_W) / 2;
    int ly = sh - TASKBAR_H - LAUNCHER_H - 8;
    int i, row, col;
    int grid_x = lx + 20;
    int grid_y = ly + 60;

    /* Background panel */
    draw_rounded_rect(dst, lx, ly, LAUNCHER_W, LAUNCHER_H, 12, COL_LAUNCH_BG);

    /* Top: OS title + search bar */
    gui_surface_draw_string(dst, lx + 20, ly + 16, "Lyth OS", COL_LAUNCH_TEXT, 0, 0);

    /* Search bar */
    {
        int sx = lx + 100, sy = ly + 12, swidth = LAUNCHER_W - 140, sheight = 24;
        draw_rounded_rect(dst, sx, sy, swidth, sheight, 6, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, sx + 10, sy + 4, "Search...", COL_LAUNCH_DIM, 0, 0);
    }

    /* Separator */
    gui_surface_hline(dst, lx + 16, ly + 48, LAUNCHER_W - 32, COL_LAUNCH_SEP);

    /* App grid */
    for (i = 0; i < launcher_item_count; i++) {
        row = i / LAUNCHER_COLS;
        col = i % LAUNCHER_COLS;

        int cx = grid_x + col * LAUNCHER_CELL_W;
        int cy = grid_y + row * LAUNCHER_CELL_H;

        /* Hover highlight for selected item */
        if (i == menu_selected) {
            draw_rounded_rect(dst, cx, cy, LAUNCHER_CELL_W - 4,
                              LAUNCHER_CELL_H - 4, 6, COL_LAUNCH_HOVER);
        }

        /* Icon (colored rounded square) */
        {
            int icon_x = cx + (LAUNCHER_CELL_W - 4 - LAUNCHER_ICON_SZ) / 2;
            int icon_y = cy + 4;
            draw_rounded_rect(dst, icon_x, icon_y, LAUNCHER_ICON_SZ,
                              LAUNCHER_ICON_SZ, 8, launcher_items[i].icon_color);
            /* Letter inside icon */
            gui_surface_draw_char(dst,
                icon_x + (LAUNCHER_ICON_SZ - FONT_PSF_WIDTH) / 2,
                icon_y + (LAUNCHER_ICON_SZ - FONT_PSF_HEIGHT) / 2,
                (unsigned char)launcher_items[i].icon_letter,
                0xFFFFFF, 0, 0);
        }

        /* Label below icon */
        {
            int lbl_w = (int)strlen(launcher_items[i].label) * FONT_PSF_WIDTH;
            int lbl_x = cx + (LAUNCHER_CELL_W - 4 - lbl_w) / 2;
            if (lbl_x < cx) lbl_x = cx;
            gui_surface_draw_string_n(dst, lbl_x,
                cy + LAUNCHER_ICON_SZ + 10,
                launcher_items[i].label,
                (LAUNCHER_CELL_W - 4) / FONT_PSF_WIDTH,
                (i == menu_selected) ? 0xFFFFFF : COL_LAUNCH_TEXT, 0, 0);
        }
    }

    /* Bottom row: Logout | Restart | Shutdown */
    {
        int by = ly + LAUNCHER_H - 36;
        gui_surface_hline(dst, lx + 16, by - 8, LAUNCHER_W - 32, COL_LAUNCH_SEP);

        int bx = lx + 20;
        /* Logout */
        draw_rounded_rect(dst, bx, by, 80, 24, 4, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 12, by + 4, "Logout", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Restart */
        draw_rounded_rect(dst, bx, by, 80, 24, 4, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 8, by + 4, "Restart", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Shutdown */
        draw_rounded_rect(dst, bx, by, 90, 24, 4, 0xF03E3E);
        gui_surface_draw_string(dst, bx + 6, by + 4, "Shutdown", 0xFFFFFF, 0, 0);
    }
}

static void draw_context_menu(gui_surface_t* dst) {
    static const char* ctx_labels[CTX_MAX_ITEMS] = { "Close", "Minimize", "Maximize" };
    int i;
    int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;

    draw_rounded_rect(dst, ctx_menu_x, ctx_menu_y, CTX_W, total_h, 6, COL_CTX_BG);
    /* border lines */
    gui_surface_hline(dst, ctx_menu_x, ctx_menu_y, CTX_W, COL_CTX_BORDER);
    gui_surface_hline(dst, ctx_menu_x, ctx_menu_y + total_h - 1, CTX_W, COL_CTX_BORDER);

    for (i = 0; i < CTX_MAX_ITEMS; i++) {
        int iy = ctx_menu_y + 2 + i * CTX_ITEM_H;
        gui_surface_draw_string(dst, ctx_menu_x + 12, iy + (CTX_ITEM_H - FONT_PSF_HEIGHT) / 2,
                                ctx_labels[i], COL_CTX_TEXT, 0, 0);
        if (i < CTX_MAX_ITEMS - 1)
            gui_surface_hline(dst, ctx_menu_x + 4, iy + CTX_ITEM_H - 1, CTX_W - 8, COL_CTX_BORDER);
    }
}

static void close_start_menu(void) {
    if (!start_menu_open) return;
    int lx = (sw - LAUNCHER_W) / 2;
    int ly = sh - TASKBAR_H - LAUNCHER_H - 8;
    start_menu_open = 0;
    menu_selected = -1;
    gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + 8);
}

static void close_context_menu(void) {
    if (!ctx_menu_open) return;
    int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
    gui_dirty_add(ctx_menu_x, ctx_menu_y, CTX_W, total_h);
    ctx_menu_open = 0;
    ctx_target_win = 0;
}

static void close_net_popup(void) {
    if (!net_popup_open) return;
    int px = sw - NET_POPUP_W - 8;
    int py = sh - TASKBAR_H - NET_POPUP_H - 8;
    gui_dirty_add(px, py, NET_POPUP_W, NET_POPUP_H);
    net_popup_open = 0;
}

static void draw_net_popup(gui_surface_t* dst) {
    int px = sw - NET_POPUP_W - 8;
    int py = sh - TASKBAR_H - NET_POPUP_H - 8;
    int ly;
    netif_t* iface = netif_get(0);

    draw_rounded_rect(dst, px, py, NET_POPUP_W, NET_POPUP_H, 10, COL_POPUP_BG);
    /* border accent line at top */
    gui_surface_hline(dst, px + 10, py, NET_POPUP_W - 20, COL_POPUP_BORDER);

    /* title */
    gui_surface_draw_string(dst, px + 10, py + 8, "Network", COL_POPUP_TEXT, 0, 0);
    gui_surface_hline(dst, px + 8, py + 8 + FONT_PSF_HEIGHT + 4, NET_POPUP_W - 16, COL_POPUP_BORDER);

    ly = py + 8 + FONT_PSF_HEIGHT + 10;

    if (!iface) {
        gui_surface_draw_string(dst, px + 10, ly, "No interface", COL_POPUP_DIM, 0, 0);
    } else {
        int connected = (iface->up && iface->ip_addr != 0);
        /* status line */
        gui_surface_draw_string(dst, px + 10, ly, "Ethernet:", COL_POPUP_TEXT, 0, 0);
        gui_surface_draw_string(dst, px + 10 + 10 * FONT_PSF_WIDTH, ly,
            connected ? "Connected" : "Disconnected",
            connected ? COL_NET_GREEN : COL_NET_RED, 0, 0);
        ly += FONT_PSF_HEIGHT + 6;

        gui_surface_draw_string(dst, px + 10, ly, "Interface:", COL_POPUP_DIM, 0, 0);
        gui_surface_draw_string(dst, px + 10 + 11 * FONT_PSF_WIDTH, ly,
            iface->name, COL_POPUP_TEXT, 0, 0);
        ly += FONT_PSF_HEIGHT + 4;

        if (connected) {
            char ip_buf[16];
            uint32_t ip = iface->ip_addr;
            /* format IP as a.b.c.d */
            {
                int pos = 0, oct;
                for (oct = 0; oct < 4; oct++) {
                    uint8_t b = (uint8_t)(ip >> (oct * 8));
                    if (b >= 100) ip_buf[pos++] = '0' + b / 100;
                    if (b >= 10)  ip_buf[pos++] = '0' + (b / 10) % 10;
                    ip_buf[pos++] = '0' + b % 10;
                    if (oct < 3) ip_buf[pos++] = '.';
                }
                ip_buf[pos] = '\0';
            }
            gui_surface_draw_string(dst, px + 10, ly, "IP:", COL_POPUP_DIM, 0, 0);
            gui_surface_draw_string(dst, px + 10 + 11 * FONT_PSF_WIDTH, ly,
                ip_buf, COL_POPUP_TEXT, 0, 0);
            ly += FONT_PSF_HEIGHT + 4;
        }

        /* "Open Network Config" clickable area */
        ly += 6;
        draw_rounded_rect(dst, px + 10, ly, NET_POPUP_W - 20, 24, 4, COL_POPUP_BTN);
        gui_surface_draw_string(dst, px + 10 + (NET_POPUP_W - 20 - 18 * FONT_PSF_WIDTH) / 2,
            ly + (24 - FONT_PSF_HEIGHT) / 2,
            "Open Network Config", 0xFFFFFF, 0, 0);
    }
}

static void open_start_menu(void) {
    start_menu_open = 1;
    menu_selected = 0;
    close_context_menu();
    close_net_popup();
    {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - TASKBAR_H - LAUNCHER_H - 8;
        gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + TASKBAR_H + 8);
    }
}

static void toggle_start_menu(void) {
    if (start_menu_open) {
        close_start_menu();
    } else {
        open_start_menu();
    }
}

/* ==================================================================*/

void desktop_init(int screen_w, int screen_h) {
    sw = screen_w;
    sh = screen_h;
    start_menu_open = 0;
    menu_selected = -1;
    ctx_menu_open = 0;
    ctx_target_win = 0;
    net_popup_open = 0;
    net_last_up = 0;
    desk_valid = 0;

    gui_surface_alloc(&desk_surf, sw, sh);
    update_clock();

    /* ---- Populate dock (pinned icons in centre of taskbar) ---- */
    dock_item_count = 0;

    dock_items[dock_item_count].label = "Files";
    dock_items[dock_item_count].action = filemanager_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_FILES;
    dock_items[dock_item_count].shortcut = 'F';
    dock_item_count++;

    dock_items[dock_item_count].label = "Settings";
    dock_items[dock_item_count].action = settings_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_SETTINGS;
    dock_items[dock_item_count].shortcut = 'S';
    dock_item_count++;

    dock_items[dock_item_count].label = "Terminal";
    dock_items[dock_item_count].action = terminal_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_TERMINAL;
    dock_items[dock_item_count].shortcut = '>';
    dock_item_count++;

    dock_items[dock_item_count].label = "Notes";
    dock_items[dock_item_count].action = editor_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_EDITOR;
    dock_items[dock_item_count].shortcut = 'N';
    dock_item_count++;

    dock_items[dock_item_count].label = "Calculator";
    dock_items[dock_item_count].action = calculator_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_CALC;
    dock_items[dock_item_count].shortcut = 'C';
    dock_item_count++;

    dock_items[dock_item_count].label = "Network";
    dock_items[dock_item_count].action = netcfg_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_NETCFG;
    dock_items[dock_item_count].shortcut = 'W';
    dock_item_count++;

    dock_items[dock_item_count].label = "Viewer";
    dock_items[dock_item_count].action = viewer_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_VIEWER;
    dock_items[dock_item_count].shortcut = 'V';
    dock_item_count++;

    dock_items[dock_item_count].label = "Task Manager";
    dock_items[dock_item_count].action = taskman_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_TASKMAN;
    dock_items[dock_item_count].shortcut = 'T';
    dock_item_count++;

    dock_items[dock_item_count].label = "About";
    dock_items[dock_item_count].action = about_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_ABOUT;
    dock_items[dock_item_count].shortcut = 'A';
    dock_item_count++;

    /* ---- Populate launcher (full app list shown in grid) ---- */
    launcher_item_count = 0;

#define ADD_LAUNCHER(lbl, fn, col, ch) do { \
    str_copy(launcher_items[launcher_item_count].label, (lbl), 24); \
    launcher_items[launcher_item_count].action = (fn); \
    launcher_items[launcher_item_count].icon_color = (col); \
    launcher_items[launcher_item_count].icon_letter = (ch); \
    launcher_item_count++; \
} while(0)

    ADD_LAUNCHER("Files",       filemanager_app_open, COL_ICON_FILES,    'F');
    ADD_LAUNCHER("Terminal",    terminal_app_open,    COL_ICON_TERMINAL, '>');
    ADD_LAUNCHER("Notes",       editor_app_open,      COL_ICON_EDITOR,   'N');
    ADD_LAUNCHER("Calculator",  calculator_app_open,  COL_ICON_CALC,     'C');
    ADD_LAUNCHER("Settings",    settings_app_open,    COL_ICON_SETTINGS, 'S');
    ADD_LAUNCHER("Viewer",      viewer_app_open,      COL_ICON_VIEWER,   'V');
    ADD_LAUNCHER("Task Mgr",    taskman_app_open,     COL_ICON_TASKMAN,  'T');
    ADD_LAUNCHER("Network",     netcfg_app_open,      COL_ICON_NETCFG,   'W');
    ADD_LAUNCHER("Sys Info",    sysinfo_app_open,     COL_ICON_SYSINFO,  'I');
    ADD_LAUNCHER("About",       about_app_open,       COL_ICON_ABOUT,    'A');

#undef ADD_LAUNCHER

    rebuild_desktop();
}

void desktop_shutdown(void) {
    gui_surface_free(&desk_surf);
}

void desktop_paint_region(gui_surface_t* dst, int x0, int y0, int x1, int y1) {
    int row, w;
    if (!desk_surf.pixels || !dst || !dst->pixels) return;

    if (!desk_valid) rebuild_desktop();

    /* clip */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > sw) x1 = sw;
    if (y1 > sh) y1 = sh;
    w = x1 - x0;
    if (w <= 0) return;

    for (row = y0; row < y1; row++) {
        memcpy(&dst->pixels[row * dst->stride + x0],
               &desk_surf.pixels[row * sw + x0],
               (size_t)w * 4);
    }

    /* overlay start menu (launcher) if open and intersects dirty region */
    if (start_menu_open) {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - TASKBAR_H - LAUNCHER_H - 8;
        if (!(lx + LAUNCHER_W <= x0 || x1 <= lx ||
              ly + LAUNCHER_H <= y0 || y1 <= ly))
            draw_start_menu(dst);
    }

    /* overlay context menu if open and intersects dirty region */
    if (ctx_menu_open) {
        int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
        if (!(ctx_menu_x + CTX_W <= x0 || x1 <= ctx_menu_x ||
              ctx_menu_y + total_h <= y0 || y1 <= ctx_menu_y))
            draw_context_menu(dst);
    }

    /* overlay network popup if open and intersects dirty region */
    if (net_popup_open) {
        int npx = sw - NET_POPUP_W - 8;
        int npy = sh - TASKBAR_H - NET_POPUP_H - 8;
        if (!(npx + NET_POPUP_W <= x0 || x1 <= npx ||
              npy + NET_POPUP_H <= y0 || y1 <= npy))
            draw_net_popup(dst);
    }
}

void desktop_on_tick(void) {
    unsigned int old_sec = clock_str[4]; /* check last minute digit */
    int net_now = net_is_connected();
    update_clock();
    if (clock_str[4] != (char)old_sec || net_now != net_last_up) {
        desk_valid = 0;
        rebuild_desktop();
        /* dirty entire taskbar since dock can show running dots */
        gui_dirty_add(0, sh - TASKBAR_H, sw, TASKBAR_H);
    }
}

int desktop_handle_click(int mx, int my, int button) {
    int taskbar_y = sh - TASKBAR_H;

    /* handle net popup clicks */
    if (net_popup_open) {
        int px = sw - NET_POPUP_W - 8;
        int py = sh - TASKBAR_H - NET_POPUP_H - 8;
        if (mx >= px && mx < px + NET_POPUP_W &&
            my >= py && my < py + NET_POPUP_H) {
            netif_t* iface = netif_get(0);
            if (iface && button == 1) {
                int connected = (iface->up && iface->ip_addr != 0);
                int ly = py + 8 + FONT_PSF_HEIGHT + 10;
                ly += FONT_PSF_HEIGHT + 6;
                ly += FONT_PSF_HEIGHT + 4;
                if (connected) ly += FONT_PSF_HEIGHT + 4;
                ly += 6;
                if (my >= ly && my < ly + 24 && mx >= px + 10 && mx < px + NET_POPUP_W - 10) {
                    close_net_popup();
                    netcfg_app_open();
                    return 1;
                }
            }
            return 1;
        }
        close_net_popup();
    }

    /* close context menu if clicking outside it */
    if (ctx_menu_open) {
        int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
        if (mx >= ctx_menu_x && mx < ctx_menu_x + CTX_W &&
            my >= ctx_menu_y && my < ctx_menu_y + total_h) {
            if (button == 1 && ctx_target_win) {
                int idx = (my - ctx_menu_y - 2) / CTX_ITEM_H;
                gui_window_t* target = ctx_target_win;
                close_context_menu();
                if (idx >= 0 && idx < CTX_MAX_ITEMS && target) {
                    if (idx == 0) {
                        gui_dirty_add(target->x - 6, target->y - 6,
                                      target->width + 12, target->height + 12);
                        if (target->on_close) target->on_close(target);
                        else gui_window_destroy(target);
                    } else if (idx == 1) {
                        target->flags |= GUI_WIN_MINIMIZED;
                        gui_dirty_add(target->x, target->y,
                                      target->width, target->height);
                        desk_valid = 0;
                        gui_dirty_add(0, taskbar_y, sw, TASKBAR_H);
                    } else if (idx == 2) {
                        gui_dirty_add(target->x, target->y,
                                      target->width, target->height);
                        gui_window_move(target, 0, 0);
                        target->width = sw;
                        target->height = sh - TASKBAR_H;
                        gui_surface_free(&target->surface);
                        gui_surface_alloc(&target->surface, target->width, target->height);
                        target->needs_redraw = 1;
                        gui_dirty_add(0, 0, sw, sh);
                    }
                }
            }
            return 1;
        }
        close_context_menu();
    }

    /* ---- Launcher (app menu) click handling ---- */
    if (start_menu_open) {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - TASKBAR_H - LAUNCHER_H - 8;

        if (mx >= lx && mx < lx + LAUNCHER_W &&
            my >= ly && my < ly + LAUNCHER_H) {

            /* Check bottom buttons: Logout | Restart | Shutdown */
            {
                int by = ly + LAUNCHER_H - 36;
                int bx = lx + 20;
                if (my >= by && my < by + 24 && button == 1) {
                    if (mx >= bx && mx < bx + 80) {
                        close_start_menu();
                        login_manager_request_logout();
                        return 1;
                    }
                    bx += 90;
                    if (mx >= bx && mx < bx + 80) {
                        close_start_menu();
                        power_reboot();
                        return 1;
                    }
                    bx += 90;
                    if (mx >= bx && mx < bx + 90) {
                        close_start_menu();
                        power_shutdown();
                        return 1;
                    }
                }
            }

            /* Check app grid */
            if (button == 1) {
                int grid_x = lx + 20;
                int grid_y = ly + 60;
                int i;
                for (i = 0; i < launcher_item_count; i++) {
                    int row = i / LAUNCHER_COLS;
                    int col = i % LAUNCHER_COLS;
                    int cx = grid_x + col * LAUNCHER_CELL_W;
                    int cy = grid_y + row * LAUNCHER_CELL_H;
                    if (mx >= cx && mx < cx + LAUNCHER_CELL_W - 4 &&
                        my >= cy && my < cy + LAUNCHER_CELL_H - 4) {
                        close_start_menu();
                        if (launcher_items[i].action)
                            launcher_items[i].action();
                        return 1;
                    }
                }
            }
            return 1; /* consumed: inside launcher */
        }
        /* click outside launcher: close it */
        close_start_menu();
    }

    /* ---- Taskbar clicks ---- */
    if (my >= taskbar_y) {
        /* Left section: running app labels */
        {
            int lx = 8;
            int i, count = gui_window_count();
            for (i = 0; i < count; i++) {
                gui_window_t* w = gui_window_get(i);
                if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
                if (w->flags & GUI_WIN_MINIMIZED) continue;
                int tlen = (int)strlen(w->title);
                if (tlen > 12) tlen = 12;
                int item_w = tlen * FONT_PSF_WIDTH + 16;
                if (lx + item_w > dock_start_x() - 20) break;

                if (mx >= lx && mx < lx + item_w) {
                    if (button == 2) {
                        close_start_menu();
                        ctx_menu_open = 1;
                        ctx_target_win = w;
                        ctx_menu_x = mx;
                        {
                            int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
                            ctx_menu_y = taskbar_y - total_h;
                        }
                        if (ctx_menu_x + CTX_W > sw) ctx_menu_x = sw - CTX_W;
                        gui_dirty_add(ctx_menu_x, ctx_menu_y, CTX_W,
                                      CTX_MAX_ITEMS * CTX_ITEM_H + 4);
                        return 1;
                    }
                    gui_window_focus(w);
                    desk_valid = 0;
                    gui_dirty_add(0, taskbar_y, sw, TASKBAR_H);
                    gui_dirty_add(w->x, w->y, w->width, w->height);
                    return 1;
                }
                lx += item_w + 6;
            }
        }

        /* Center: dock icon clicks */
        {
            int dx = dock_start_x();
            int dy = taskbar_y + DOCK_Y_PAD;
            int icon_h = TASKBAR_H - DOCK_Y_PAD * 2;
            int i;
            for (i = 0; i < dock_item_count; i++) {
                int ix = dx + DOCK_ICON_PAD + i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
                int iy = dy + (icon_h - DOCK_ICON_SIZE) / 2;
                if (mx >= ix && mx < ix + DOCK_ICON_SIZE &&
                    my >= iy && my < iy + DOCK_ICON_SIZE && button == 1) {
                    if (dock_items[i].action)
                        dock_items[i].action();
                    return 1;
                }
            }
        }

        /* Right section: tray area — network icon region opens popup */
        {
            /* The tray spans the rightmost ~200px. Network icon is roughly in the middle */
            int tray_x = sw - CLOCK_DATE_W - TRAY_PAD * 3 - 36;
            if (mx >= tray_x && mx < tray_x + 20 && button == 1) {
                close_start_menu();
                close_context_menu();
                if (net_popup_open) {
                    close_net_popup();
                } else {
                    net_popup_open = 1;
                    int px = sw - NET_POPUP_W - 8;
                    int py = sh - TASKBAR_H - NET_POPUP_H - 8;
                    gui_dirty_add(px, py, NET_POPUP_W, NET_POPUP_H);
                }
                return 1;
            }
        }

        /* Clicking on the hamburger/menu icon area toggles start menu */
        {
            int menu_x = sw - CLOCK_DATE_W - TRAY_PAD * 4 - 48;
            if (mx >= menu_x && mx < menu_x + 20 && button == 1) {
                toggle_start_menu();
                return 1;
            }
        }

        return 1; /* consumed: click was on taskbar */
    }

    return 0;
}

int desktop_handle_key(int event_type, char key) {
    /* Super / Windows key: toggle start menu */
    if (event_type == INPUT_EVENT_SUPER) {
        toggle_start_menu();
        return 1;
    }

    /* Launcher keyboard navigation (grid-based) */
    if (start_menu_open) {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - TASKBAR_H - LAUNCHER_H - 8;

        if (event_type == INPUT_EVENT_UP) {
            if (menu_selected >= LAUNCHER_COLS) menu_selected -= LAUNCHER_COLS;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_DOWN) {
            if (menu_selected + LAUNCHER_COLS < launcher_item_count)
                menu_selected += LAUNCHER_COLS;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_LEFT) {
            if (menu_selected > 0) menu_selected--;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_RIGHT) {
            if (menu_selected < launcher_item_count - 1) menu_selected++;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_ENTER) {
            if (menu_selected >= 0 && menu_selected < launcher_item_count) {
                void (*action)(void) = launcher_items[menu_selected].action;
                close_start_menu();
                gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + TASKBAR_H + 8);
                if (action) action();
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_CHAR && key == 27) {
            close_start_menu();
            return 1;
        }
    }

    /* Escape closes net popup */
    if (net_popup_open && event_type == INPUT_EVENT_CHAR && key == 27) {
        close_net_popup();
        return 1;
    }

    return 0;
}

int desktop_get_menubar_height(void) {
    return 0;
}

gui_surface_t* desktop_get_surface(void) {
    return &desk_surf;
}

void desktop_invalidate_taskbar(void) {
    desk_valid = 0;
    gui_dirty_add(0, sh - TASKBAR_H, sw, TASKBAR_H);
}
