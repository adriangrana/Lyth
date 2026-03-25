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
#include "session.h"
#include "lib/png.h"
#include "wallpaper_png.h"
#include "wallpaper_sky_png.h"

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

/* ---- context menu (window right-click) ---- */
#define CTX_W           140
#define CTX_ITEM_H      24
#define CTX_MAX_ITEMS   3

static int ctx_menu_open;
static int ctx_menu_x, ctx_menu_y;
static gui_window_t* ctx_target_win;

/* ---- desktop context menu (right-click on desktop background) ---- */
#define DCTX_W          180
#define DCTX_ITEM_H     28
#define DCTX_MAX_ITEMS  5

typedef struct {
    const char* label;
    void (*action)(void);
} dctx_item_t;

static int dctx_menu_open;
static int dctx_menu_x, dctx_menu_y;
static int dctx_hover;
static dctx_item_t dctx_items[DCTX_MAX_ITEMS];
static int dctx_item_count;

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

/* ---- launcher search ---- */
static char launcher_search[32];
static int launcher_search_len;

/* ---- tray click regions (set during render) ---- */
static int tray_net_x, tray_menu_x;

/* ---- wallpaper catalogue ---- */
#define WP_TYPE_IMAGE   0
#define WP_TYPE_SOLID   1
#define WP_MAX          8

typedef struct {
    const char *name;
    int         type;          /* WP_TYPE_IMAGE or WP_TYPE_SOLID */
    const uint8_t *png_data;   /* for image wallpapers */
    unsigned int   png_size;
    uint32_t    solid_col;     /* for solid colour wallpapers */
    png_image_t  decoded;      /* cached decoded image (pre-decoded at init) */
    int          decoded_ok;   /* 1 = decoded successfully */
} wp_entry_t;

static wp_entry_t wp_catalogue[WP_MAX];
static int wp_count = 0;
static int wp_selected = 0;   /* index into catalogue */

static png_image_t wallpaper_img;
static int wallpaper_loaded = 0;
static int wallpaper_current = -1;  /* which catalogue entry is loaded */

static void wallpaper_catalogue_init(void) {
    int i;
    if (wp_count > 0) return;
    /* Image wallpapers from defaults_wallpapers/ */
    wp_catalogue[wp_count].name = "Lyth";
    wp_catalogue[wp_count].type = WP_TYPE_IMAGE;
    wp_catalogue[wp_count].png_data = wallpaper_png_data;
    wp_catalogue[wp_count].png_size = (unsigned int)wallpaper_png_size;
    wp_count++;

    wp_catalogue[wp_count].name = "Sky";
    wp_catalogue[wp_count].type = WP_TYPE_IMAGE;
    wp_catalogue[wp_count].png_data = wallpaper_sky_png_data;
    wp_catalogue[wp_count].png_size = (unsigned int)wallpaper_sky_png_size;
    wp_count++;

    /* Solid colour wallpapers */
    wp_catalogue[wp_count].name = "Midnight";
    wp_catalogue[wp_count].type = WP_TYPE_SOLID;
    wp_catalogue[wp_count].solid_col = 0x0D1117;
    wp_count++;

    wp_catalogue[wp_count].name = "Ocean";
    wp_catalogue[wp_count].type = WP_TYPE_SOLID;
    wp_catalogue[wp_count].solid_col = 0x1E3A5F;
    wp_count++;

    wp_catalogue[wp_count].name = "Forest";
    wp_catalogue[wp_count].type = WP_TYPE_SOLID;
    wp_catalogue[wp_count].solid_col = 0x1A3A2A;
    wp_count++;

    wp_catalogue[wp_count].name = "Sunset";
    wp_catalogue[wp_count].type = WP_TYPE_SOLID;
    wp_catalogue[wp_count].solid_col = 0x5A2D2D;
    wp_count++;

    /* Pre-decode all image wallpapers so thumbnails and apply are instant */
    for (i = 0; i < wp_count; i++) {
        if (wp_catalogue[i].type == WP_TYPE_IMAGE && wp_catalogue[i].png_data) {
            if (png_load(wp_catalogue[i].png_data, wp_catalogue[i].png_size,
                         &wp_catalogue[i].decoded) == 0)
                wp_catalogue[i].decoded_ok = 1;
        }
    }
}

static void try_load_wallpaper(void) {
    wp_entry_t *e;
    wallpaper_catalogue_init();
    if (wp_selected == wallpaper_current && wallpaper_loaded) return;
    wallpaper_loaded = 1;
    wallpaper_current = wp_selected;
    e = &wp_catalogue[wp_selected];
    if (e->type == WP_TYPE_IMAGE && e->decoded_ok) {
        /* Use pre-decoded cache — instant, no PNG decode needed */
        wallpaper_img = e->decoded;
    } else {
        /* Solid or failed decode: no image pixels */
        wallpaper_img.pixels = 0;
    }
}

/* Bilinear interpolation for smooth wallpaper scaling (16.16 fixed-point) */
static uint32_t bilinear_sample_wp(const uint32_t *pixels, int iw, int ih,
                                    int fx16, int fy16)
{
    int x0 = fx16 >> 16;
    int y0 = fy16 >> 16;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    int fx = fx16 & 0xFFFF;
    int fy = fy16 & 0xFFFF;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= iw) x1 = iw - 1;
    if (y1 >= ih) y1 = ih - 1;
    if (x0 >= iw) x0 = iw - 1;
    if (y0 >= ih) y0 = ih - 1;

    uint32_t c00 = pixels[y0 * iw + x0];
    uint32_t c10 = pixels[y0 * iw + x1];
    uint32_t c01 = pixels[y1 * iw + x0];
    uint32_t c11 = pixels[y1 * iw + x1];

    int ifx = 0x10000 - fx;
    int ify = 0x10000 - fy;

    int r = (int)( (((c00>>16)&0xFF)*ifx + ((c10>>16)&0xFF)*fx) >> 16 ) * ify
          + (int)( (((c01>>16)&0xFF)*ifx + ((c11>>16)&0xFF)*fx) >> 16 ) * fy;
    int g = (int)( (((c00>>8)&0xFF)*ifx + ((c10>>8)&0xFF)*fx) >> 16 ) * ify
          + (int)( (((c01>>8)&0xFF)*ifx + ((c11>>8)&0xFF)*fx) >> 16 ) * fy;
    int b = (int)( ((c00&0xFF)*ifx + (c10&0xFF)*fx) >> 16 ) * ify
          + (int)( ((c01&0xFF)*ifx + (c11&0xFF)*fx) >> 16 ) * fy;

    r >>= 16; g >>= 16; b >>= 16;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

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

/* Alpha-blend a solid color onto existing pixels (0=transparent, 255=opaque) */
static void alpha_blend_fill(gui_surface_t* dst, int x0, int y0, int w, int h,
                              uint32_t col, int alpha) {
    int row, cx;
    uint32_t fr = (col >> 16) & 0xFF;
    uint32_t fg = (col >> 8) & 0xFF;
    uint32_t fb = col & 0xFF;
    int ia = 255 - alpha;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > dst->width) w = dst->width - x0;
    if (y0 + h > dst->height) h = dst->height - y0;
    if (w <= 0 || h <= 0) return;
    for (row = y0; row < y0 + h; row++) {
        uint32_t *p = &dst->pixels[row * dst->stride + x0];
        for (cx = 0; cx < w; cx++) {
            uint32_t bg = p[cx];
            uint32_t r = (fr * (uint32_t)alpha + ((bg >> 16) & 0xFF) * (uint32_t)ia) / 255;
            uint32_t g = (fg * (uint32_t)alpha + ((bg >> 8) & 0xFF) * (uint32_t)ia) / 255;
            uint32_t b = (fb * (uint32_t)alpha + (bg & 0xFF) * (uint32_t)ia) / 255;
            p[cx] = (r << 16) | (g << 8) | b;
        }
    }
}

/* Rounded rect with alpha blending */
static void draw_rounded_rect_alpha(gui_surface_t* dst, int x, int y, int w,
                                     int h, int r, uint32_t col, int alpha) {
    if (alpha >= 255) { draw_rounded_rect(dst, x, y, w, h, r, col); return; }
    if (alpha <= 0) return;
    if (r < 1 || h < 4 || w < 4) {
        alpha_blend_fill(dst, x, y, w, h, col, alpha);
        return;
    }
    if (r > 3) r = 3;
    if (r == 1) {
        alpha_blend_fill(dst, x + 1, y, w - 2, 1, col, alpha);
        alpha_blend_fill(dst, x, y + 1, w, h - 2, col, alpha);
        alpha_blend_fill(dst, x + 1, y + h - 1, w - 2, 1, col, alpha);
    } else if (r == 2) {
        alpha_blend_fill(dst, x + 2, y, w - 4, 1, col, alpha);
        alpha_blend_fill(dst, x + 1, y + 1, w - 2, 1, col, alpha);
        alpha_blend_fill(dst, x, y + 2, w, h - 4, col, alpha);
        alpha_blend_fill(dst, x + 1, y + h - 2, w - 2, 1, col, alpha);
        alpha_blend_fill(dst, x + 2, y + h - 1, w - 4, 1, col, alpha);
    } else {
        alpha_blend_fill(dst, x + 3, y, w - 6, 1, col, alpha);
        alpha_blend_fill(dst, x + 2, y + 1, w - 4, 1, col, alpha);
        alpha_blend_fill(dst, x + 1, y + 2, w - 2, 1, col, alpha);
        alpha_blend_fill(dst, x, y + 3, w, h - 6, col, alpha);
        alpha_blend_fill(dst, x + 1, y + h - 3, w - 2, 1, col, alpha);
        alpha_blend_fill(dst, x + 2, y + h - 2, w - 4, 1, col, alpha);
        alpha_blend_fill(dst, x + 3, y + h - 1, w - 6, 1, col, alpha);
    }
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
    try_load_wallpaper();
    if (wallpaper_img.pixels && wallpaper_img.width > 0 && wallpaper_img.height > 0) {
        /* Scale/center-crop wallpaper to fill the desktop area */
        int iw = wallpaper_img.width;
        int ih = wallpaper_img.height;
        int dh = sh;   /* fill full screen including taskbar area */

        /* Use nearest-neighbor scaling (cover mode: fill, crop excess) */
        int scale_num, scale_den;
        int sx_off = 0, sy_off = 0;

        /* Pick scale so image covers entire screen */
        if (iw * dh > ih * sw) {
            /* Image wider proportionally → scale by height, crop width */
            scale_num = dh;
            scale_den = ih;
            int scaled_w = (int)((long)iw * scale_num / scale_den);
            sx_off = (scaled_w - sw) / 2;
        } else {
            /* Image taller proportionally → scale by width, crop height */
            scale_num = sw;
            scale_den = iw;
            int scaled_h = (int)((long)ih * scale_num / scale_den);
            sy_off = (scaled_h - dh) / 2;
        }

        for (y = 0; y < dh; y++) {
            int sy = (int)((long)(y + sy_off) * scale_den / scale_num);
            const uint32_t *src_row;
            uint32_t *dst_row;
            if (sy < 0) sy = 0;
            if (sy >= ih) sy = ih - 1;
            src_row = &wallpaper_img.pixels[sy * iw];
            dst_row = &desk_surf.pixels[y * sw];
            for (int x = 0; x < sw; x++) {
                int sxi = (int)((long)(x + sx_off) * scale_den / scale_num);
                if (sxi < 0) sxi = 0;
                if (sxi >= iw) sxi = iw - 1;
                dst_row[x] = src_row[sxi];
            }
        }
    } else {
        /* Solid colour or gradient fallback */
        uint32_t solid = 0;
        int use_solid = 0;
        wallpaper_catalogue_init();
        if (wp_selected < wp_count && wp_catalogue[wp_selected].type == WP_TYPE_SOLID) {
            solid = wp_catalogue[wp_selected].solid_col;
            use_solid = 1;
        }
        if (use_solid) {
            for (y = 0; y < sh; y++)
                memset32(&desk_surf.pixels[y * sw], solid, (size_t)sw);
        } else {
            /* gradient fallback */
            for (y = 0; y < half; y++) {
                uint32_t c = mix_rgb(COL_BG_TOP, COL_BG_MID, y, half > 1 ? half - 1 : 1);
                memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
            }
            for (y = half; y < sh - TASKBAR_H; y++) {
                uint32_t c = mix_rgb(COL_BG_MID, COL_BG_BOT, y - half,
                                     (sh - TASKBAR_H - half) > 1 ? (sh - TASKBAR_H - half - 1) : 1);
                memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
            }
        }
    }

    /* ---- Taskbar ---- */
    taskbar_y = sh - TASKBAR_H;

    /* Taskbar background — translucent glass over wallpaper */
    alpha_blend_fill(&desk_surf, 0, taskbar_y, sw, TASKBAR_H, 0x0D1117, 190);
    /* Subtle accent line at top edge */
    alpha_blend_fill(&desk_surf, 0, taskbar_y, sw, 1, THEME_COL_ACCENT, 30);

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

        /* Dock background pill — translucent */
        draw_rounded_rect_alpha(&desk_surf, dx - 6, taskbar_y + 2,
                                dock_total_w() + 12, TASKBAR_H - 4, 3, COL_DOCK_BG, 160);

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
                        /* running indicator bar below icon */
                        gui_surface_fill(&desk_surf,
                            ix + DOCK_ICON_SIZE / 2 - 3,
                            taskbar_y + TASKBAR_H - 4,
                            6, 2, COL_DOCK_DOT);
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

        /* Separator */
        alpha_blend_fill(&desk_surf, rx, taskbar_y + 8, 1, TASKBAR_H - 16,
                         COL_TASKBAR_SEP, 120);
        rx -= TRAY_PAD;

        /* Username from session */
        {
            const session_t* sess = session_get_current();
            if (sess && sess->active && sess->username[0]) {
                int ulen = (int)strlen(sess->username);
                int uw = ulen * FONT_PSF_WIDTH;
                rx -= uw;
                gui_surface_draw_string(&desk_surf, rx,
                    taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
                    sess->username, COL_TRAY_TEXT, 0, 0);
                rx -= TRAY_PAD;
                alpha_blend_fill(&desk_surf, rx, taskbar_y + 8, 1,
                                 TASKBAR_H - 16, COL_TASKBAR_SEP, 120);
                rx -= TRAY_PAD;
            }
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
            tray_net_x = rx;
            draw_net_icon(&desk_surf, rx, taskbar_y + (TASKBAR_H - 9) / 2,
                          connected);
            rx -= TRAY_PAD;
        }

        /* Separator */
        alpha_blend_fill(&desk_surf, rx, taskbar_y + 8, 1, TASKBAR_H - 16,
                         COL_TASKBAR_SEP, 120);
        rx -= TRAY_PAD;

        /* Menu/hamburger icon */
        rx -= 12;
        tray_menu_x = rx;
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

    /* Build filtered list based on search */
    int filtered[LAUNCHER_MAX];
    int fcount = 0;
    for (i = 0; i < launcher_item_count; i++) {
        if (launcher_search_len == 0 ||
            str_starts_with_ignore_case(launcher_items[i].label, launcher_search))
            filtered[fcount++] = i;
    }

    /* Background panel — translucent glass */
    draw_rounded_rect_alpha(dst, lx, ly, LAUNCHER_W, LAUNCHER_H, 3, COL_LAUNCH_BG, 210);
    /* Subtle accent border at top */
    alpha_blend_fill(dst, lx + 3, ly, LAUNCHER_W - 6, 1, THEME_COL_ACCENT, 25);

    /* Top: OS title + search bar */
    gui_surface_draw_string(dst, lx + 20, ly + 16, "Lyth OS", THEME_COL_ACCENT, 0, 0);

    /* Search bar */
    {
        int sx = lx + 100, sy = ly + 12, swidth = LAUNCHER_W - 140, sheight = 24;
        draw_rounded_rect(dst, sx, sy, swidth, sheight, 2, COL_LAUNCH_SEARCH_BG);
        /* Focus ring when search is active */
        if (launcher_search_len > 0)
            alpha_blend_fill(dst, sx, sy, swidth, 1, THEME_COL_ACCENT, 80);
        if (launcher_search_len > 0) {
            gui_surface_draw_string_n(dst, sx + 10, sy + 4,
                launcher_search, 30, COL_LAUNCH_TEXT, 0, 0);
            /* Cursor blink (simple solid bar) */
            gui_surface_fill(dst, sx + 10 + launcher_search_len * FONT_PSF_WIDTH,
                sy + 4, 1, FONT_PSF_HEIGHT, COL_LAUNCH_TEXT);
        } else {
            gui_surface_draw_string(dst, sx + 10, sy + 4, "Search...", COL_LAUNCH_DIM, 0, 0);
        }
    }

    /* Separator */
    alpha_blend_fill(dst, lx + 16, ly + 48, LAUNCHER_W - 32, 1, COL_LAUNCH_SEP, 150);

    /* App grid (filtered) */
    for (i = 0; i < fcount; i++) {
        int idx = filtered[i];
        row = i / LAUNCHER_COLS;
        col = i % LAUNCHER_COLS;

        int cx = grid_x + col * LAUNCHER_CELL_W;
        int cy = grid_y + row * LAUNCHER_CELL_H;

        /* Hover highlight for selected item */
        if (i == menu_selected) {
            draw_rounded_rect_alpha(dst, cx, cy, LAUNCHER_CELL_W - 4,
                              LAUNCHER_CELL_H - 4, 2, COL_LAUNCH_HOVER, 60);
        }

        /* Icon (colored rounded square) */
        {
            int icon_x = cx + (LAUNCHER_CELL_W - 4 - LAUNCHER_ICON_SZ) / 2;
            int icon_y = cy + 4;
            draw_rounded_rect(dst, icon_x, icon_y, LAUNCHER_ICON_SZ,
                              LAUNCHER_ICON_SZ, 3, launcher_items[idx].icon_color);
            /* Letter inside icon */
            gui_surface_draw_char(dst,
                icon_x + (LAUNCHER_ICON_SZ - FONT_PSF_WIDTH) / 2,
                icon_y + (LAUNCHER_ICON_SZ - FONT_PSF_HEIGHT) / 2,
                (unsigned char)launcher_items[idx].icon_letter,
                0xFFFFFF, 0, 0);
        }

        /* Label below icon */
        {
            int lbl_w = (int)strlen(launcher_items[idx].label) * FONT_PSF_WIDTH;
            int lbl_x = cx + (LAUNCHER_CELL_W - 4 - lbl_w) / 2;
            if (lbl_x < cx) lbl_x = cx;
            gui_surface_draw_string_n(dst, lbl_x,
                cy + LAUNCHER_ICON_SZ + 10,
                launcher_items[idx].label,
                (LAUNCHER_CELL_W - 4) / FONT_PSF_WIDTH,
                (i == menu_selected) ? 0xFFFFFF : COL_LAUNCH_TEXT, 0, 0);
        }
    }

    /* "No results" message when search returns empty */
    if (fcount == 0 && launcher_search_len > 0) {
        gui_surface_draw_string(dst, lx + (LAUNCHER_W - 12 * FONT_PSF_WIDTH) / 2,
            grid_y + 30, "No results.", COL_LAUNCH_DIM, 0, 0);
    }

    /* Bottom row: Logout | Restart | Shutdown */
    {
        int by = ly + LAUNCHER_H - 36;
        alpha_blend_fill(dst, lx + 16, by - 8, LAUNCHER_W - 32, 1,
                         COL_LAUNCH_SEP, 150);

        int bx = lx + 20;
        /* Logout */
        draw_rounded_rect(dst, bx, by, 80, 24, 2, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 12, by + 4, "Logout", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Restart */
        draw_rounded_rect(dst, bx, by, 80, 24, 2, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 8, by + 4, "Restart", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Shutdown */
        draw_rounded_rect(dst, bx, by, 90, 24, 2, THEME_COL_ERROR);
        gui_surface_draw_string(dst, bx + 6, by + 4, "Shutdown", 0xFFFFFF, 0, 0);
    }
}

static void draw_context_menu(gui_surface_t* dst) {
    static const char* ctx_labels[CTX_MAX_ITEMS] = { "Close", "Minimize", "Maximize" };
    int i;
    int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;

    draw_rounded_rect_alpha(dst, ctx_menu_x, ctx_menu_y, CTX_W, total_h, 3, COL_CTX_BG, 220);
    /* border lines */
    alpha_blend_fill(dst, ctx_menu_x, ctx_menu_y, CTX_W, 1, COL_CTX_BORDER, 150);
    alpha_blend_fill(dst, ctx_menu_x, ctx_menu_y + total_h - 1, CTX_W, 1, COL_CTX_BORDER, 150);

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
    launcher_search_len = 0;
    launcher_search[0] = '\0';
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

static void close_desktop_ctx(void) {
    if (!dctx_menu_open) return;
    int total_h = dctx_item_count * DCTX_ITEM_H + 8;
    gui_dirty_add(dctx_menu_x, dctx_menu_y, DCTX_W, total_h);
    dctx_menu_open = 0;
    dctx_hover = -1;
}

static void draw_desktop_ctx(gui_surface_t* dst) {
    int i;
    int total_h = dctx_item_count * DCTX_ITEM_H + 8;

    /* Translucent background */
    draw_rounded_rect_alpha(dst, dctx_menu_x, dctx_menu_y, DCTX_W, total_h,
                            3, COL_CTX_BG, 220);
    /* Top border accent */
    alpha_blend_fill(dst, dctx_menu_x + 3, dctx_menu_y, DCTX_W - 6, 1,
                     THEME_COL_ACCENT, 35);

    for (i = 0; i < dctx_item_count; i++) {
        int iy = dctx_menu_y + 4 + i * DCTX_ITEM_H;

        /* Hover highlight */
        if (i == dctx_hover)
            draw_rounded_rect_alpha(dst, dctx_menu_x + 4, iy,
                                    DCTX_W - 8, DCTX_ITEM_H - 2, 2,
                                    COL_CTX_HOVER, 60);

        gui_surface_draw_string(dst, dctx_menu_x + 14,
            iy + (DCTX_ITEM_H - FONT_PSF_HEIGHT) / 2,
            dctx_items[i].label,
            (i == dctx_hover) ? 0xFFFFFF : COL_CTX_TEXT, 0, 0);

        /* Separator after each item except last */
        if (i < dctx_item_count - 1)
            alpha_blend_fill(dst, dctx_menu_x + 8,
                iy + DCTX_ITEM_H - 2, DCTX_W - 16, 1,
                COL_CTX_BORDER, 80);
    }
}

static void draw_net_popup(gui_surface_t* dst) {
    int px = sw - NET_POPUP_W - 8;
    int py = sh - TASKBAR_H - NET_POPUP_H - 8;
    int ly;
    netif_t* iface = netif_get(0);

    draw_rounded_rect_alpha(dst, px, py, NET_POPUP_W, NET_POPUP_H, 3, COL_POPUP_BG, 220);
    /* border accent line at top */
    alpha_blend_fill(dst, px + 3, py, NET_POPUP_W - 6, 1, THEME_COL_ACCENT, 40);

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
    launcher_search_len = 0;
    launcher_search[0] = '\0';
    tray_net_x = sw - 100;
    tray_menu_x = sw - 140;
    dctx_menu_open = 0;
    dctx_hover = -1;
    desk_valid = 0;

    gui_surface_alloc(&desk_surf, sw, sh);
    update_clock();

    /* ---- Populate desktop context menu ---- */
    dctx_item_count = 0;
    dctx_items[dctx_item_count].label = "Terminal";
    dctx_items[dctx_item_count].action = terminal_app_open;
    dctx_item_count++;
    dctx_items[dctx_item_count].label = "Files";
    dctx_items[dctx_item_count].action = filemanager_app_open;
    dctx_item_count++;
    dctx_items[dctx_item_count].label = "Settings";
    dctx_items[dctx_item_count].action = settings_app_open;
    dctx_item_count++;
    dctx_items[dctx_item_count].label = "About Lyth";
    dctx_items[dctx_item_count].action = about_app_open;
    dctx_item_count++;

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

    /* overlay context menu if open and intersects dirty region */
    if (ctx_menu_open) {
        int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
        if (!(ctx_menu_x + CTX_W <= x0 || x1 <= ctx_menu_x ||
              ctx_menu_y + total_h <= y0 || y1 <= ctx_menu_y)) {
            /* Refresh wallpaper under full overlay to prevent alpha accumulation */
            {
                int ax0 = ctx_menu_x, ay0 = ctx_menu_y;
                int ax1 = ctx_menu_x + CTX_W, ay1 = ctx_menu_y + total_h;
                int ar;
                if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0;
                if (ax1 > sw) ax1 = sw; if (ay1 > sh) ay1 = sh;
                for (ar = ay0; ar < ay1; ar++)
                    memcpy(&dst->pixels[ar * dst->stride + ax0],
                           &desk_surf.pixels[ar * sw + ax0],
                           (size_t)(ax1 - ax0) * 4);
            }
            draw_context_menu(dst);
        }
    }

    /* overlay network popup if open and intersects dirty region */
    if (net_popup_open) {
        int npx = sw - NET_POPUP_W - 8;
        int npy = sh - TASKBAR_H - NET_POPUP_H - 8;
        if (!(npx + NET_POPUP_W <= x0 || x1 <= npx ||
              npy + NET_POPUP_H <= y0 || y1 <= npy)) {
            {
                int ax0 = npx, ay0 = npy;
                int ax1 = npx + NET_POPUP_W, ay1 = npy + NET_POPUP_H;
                int ar;
                if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0;
                if (ax1 > sw) ax1 = sw; if (ay1 > sh) ay1 = sh;
                for (ar = ay0; ar < ay1; ar++)
                    memcpy(&dst->pixels[ar * dst->stride + ax0],
                           &desk_surf.pixels[ar * sw + ax0],
                           (size_t)(ax1 - ax0) * 4);
            }
            draw_net_popup(dst);
        }
    }

    /* overlay desktop context menu if open and intersects dirty region */
    if (dctx_menu_open) {
        int dth = dctx_item_count * DCTX_ITEM_H + 8;
        if (!(dctx_menu_x + DCTX_W <= x0 || x1 <= dctx_menu_x ||
              dctx_menu_y + dth <= y0 || y1 <= dctx_menu_y)) {
            {
                int ax0 = dctx_menu_x, ay0 = dctx_menu_y;
                int ax1 = dctx_menu_x + DCTX_W, ay1 = dctx_menu_y + dth;
                int ar;
                if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0;
                if (ax1 > sw) ax1 = sw; if (ay1 > sh) ay1 = sh;
                for (ar = ay0; ar < ay1; ar++)
                    memcpy(&dst->pixels[ar * dst->stride + ax0],
                           &desk_surf.pixels[ar * sw + ax0],
                           (size_t)(ax1 - ax0) * 4);
            }
            draw_desktop_ctx(dst);
        }
    }
}

/* Paint overlays that must appear on top of all windows (launcher). */
void desktop_paint_overlays(gui_surface_t* dst, int x0, int y0, int x1, int y1) {
    if (!start_menu_open) return;
    {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - TASKBAR_H - LAUNCHER_H - 8;
        if (!(lx + LAUNCHER_W <= x0 || x1 <= lx ||
              ly + LAUNCHER_H <= y0 || y1 <= ly)) {
            /* Refresh wallpaper under the full launcher area before
             * alpha-blending. Prevents accumulation artifacts when
             * multiple dirty rects per frame each trigger a full
             * launcher redraw (the alpha blend only reads a clean base). */
            {
                int ax0 = lx, ay0 = ly;
                int ax1 = lx + LAUNCHER_W, ay1 = ly + LAUNCHER_H;
                int ar;
                if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0;
                if (ax1 > sw) ax1 = sw; if (ay1 > sh) ay1 = sh;
                if (desk_surf.pixels) {
                    for (ar = ay0; ar < ay1; ar++)
                        memcpy(&dst->pixels[ar * dst->stride + ax0],
                               &desk_surf.pixels[ar * sw + ax0],
                               (size_t)(ax1 - ax0) * 4);
                }
            }
            draw_start_menu(dst);
        }
    }
}

int desktop_is_overlay_open(void) {
    return start_menu_open;
}

void desktop_close_overlays(void) {
    if (start_menu_open)
        close_start_menu();
}

/* ---- wallpaper API for settings app ---- */
int desktop_wallpaper_count(void) {
    wallpaper_catalogue_init();
    return wp_count;
}

int desktop_wallpaper_selected(void) {
    return wp_selected;
}

const char* desktop_wallpaper_name(int idx) {
    wallpaper_catalogue_init();
    if (idx < 0 || idx >= wp_count) return "";
    return wp_catalogue[idx].name;
}

int desktop_wallpaper_is_image(int idx) {
    wallpaper_catalogue_init();
    if (idx < 0 || idx >= wp_count) return 0;
    return wp_catalogue[idx].type == WP_TYPE_IMAGE;
}

uint32_t desktop_wallpaper_solid_col(int idx) {
    wallpaper_catalogue_init();
    if (idx < 0 || idx >= wp_count) return 0;
    return wp_catalogue[idx].solid_col;
}

const uint32_t* desktop_wallpaper_pixels(int idx, int *out_w, int *out_h) {
    wallpaper_catalogue_init();
    if (idx < 0 || idx >= wp_count) return 0;
    if (wp_catalogue[idx].type == WP_TYPE_IMAGE && wp_catalogue[idx].decoded_ok) {
        if (out_w) *out_w = wp_catalogue[idx].decoded.width;
        if (out_h) *out_h = wp_catalogue[idx].decoded.height;
        return wp_catalogue[idx].decoded.pixels;
    }
    return 0;
}

void desktop_set_wallpaper(int idx) {
    wallpaper_catalogue_init();
    if (idx < 0 || idx >= wp_count) return;
    wp_selected = idx;
    desk_valid = 0;
    /* Full screen dirty to repaint */
    gui_dirty_add(0, 0, sw, sh);
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

    /* handle desktop context menu clicks */
    if (dctx_menu_open) {
        int dth = dctx_item_count * DCTX_ITEM_H + 8;
        if (mx >= dctx_menu_x && mx < dctx_menu_x + DCTX_W &&
            my >= dctx_menu_y && my < dctx_menu_y + dth) {
            if (button == 1) {
                int idx = (my - dctx_menu_y - 4) / DCTX_ITEM_H;
                if (idx >= 0 && idx < dctx_item_count) {
                    void (*act)(void) = dctx_items[idx].action;
                    close_desktop_ctx();
                    if (act) act();
                }
            }
            return 1;
        }
        close_desktop_ctx();
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

            /* Check app grid (filtered) */
            if (button == 1) {
                int grid_x = lx + 20;
                int grid_y = ly + 60;
                int fi, fpos = 0;
                for (fi = 0; fi < launcher_item_count; fi++) {
                    if (launcher_search_len > 0 &&
                        !str_starts_with_ignore_case(launcher_items[fi].label,
                                                      launcher_search))
                        continue;
                    int row = fpos / LAUNCHER_COLS;
                    int col = fpos % LAUNCHER_COLS;
                    int cx = grid_x + col * LAUNCHER_CELL_W;
                    int cy = grid_y + row * LAUNCHER_CELL_H;
                    if (mx >= cx && mx < cx + LAUNCHER_CELL_W - 4 &&
                        my >= cy && my < cy + LAUNCHER_CELL_H - 4) {
                        close_start_menu();
                        if (launcher_items[fi].action)
                            launcher_items[fi].action();
                        return 1;
                    }
                    fpos++;
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

        /* Right section: tray area — network icon opens popup */
        {
            if (mx >= tray_net_x && mx < tray_net_x + 20 && button == 1) {
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
            if (mx >= tray_menu_x && mx < tray_menu_x + 20 && button == 1) {
                toggle_start_menu();
                return 1;
            }
        }

        return 1; /* consumed: click was on taskbar */
    }

    /* ---- Desktop background: right-click opens desktop context menu ---- */
    if (button == 2 && my < taskbar_y) {
        close_start_menu();
        close_context_menu();
        close_net_popup();
        close_desktop_ctx();

        dctx_menu_x = mx;
        dctx_menu_y = my;
        /* Keep menu on-screen */
        {
            int dth = dctx_item_count * DCTX_ITEM_H + 8;
            if (dctx_menu_x + DCTX_W > sw) dctx_menu_x = sw - DCTX_W;
            if (dctx_menu_y + dth > sh - TASKBAR_H) dctx_menu_y = sh - TASKBAR_H - dth;
            if (dctx_menu_x < 0) dctx_menu_x = 0;
            if (dctx_menu_y < 0) dctx_menu_y = 0;
        }
        dctx_hover = -1;
        dctx_menu_open = 1;
        gui_dirty_add(dctx_menu_x, dctx_menu_y, DCTX_W,
                      dctx_item_count * DCTX_ITEM_H + 8);
        return 1;
    }

    /* Left-click on desktop: close any open menus */
    if (button == 1) {
        close_desktop_ctx();
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

        /* Compute filtered count for navigation bounds */
        int fcount = 0;
        {
            int fi;
            for (fi = 0; fi < launcher_item_count; fi++) {
                if (launcher_search_len == 0 ||
                    str_starts_with_ignore_case(launcher_items[fi].label,
                                                 launcher_search))
                    fcount++;
            }
        }

        if (event_type == INPUT_EVENT_UP) {
            if (menu_selected >= LAUNCHER_COLS) menu_selected -= LAUNCHER_COLS;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_DOWN) {
            if (menu_selected + LAUNCHER_COLS < fcount)
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
            if (menu_selected < fcount - 1) menu_selected++;
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_ENTER) {
            /* Resolve filtered index to actual item */
            if (menu_selected >= 0 && menu_selected < fcount) {
                int fi, cnt = 0, actual = -1;
                for (fi = 0; fi < launcher_item_count; fi++) {
                    if (launcher_search_len == 0 ||
                        str_starts_with_ignore_case(launcher_items[fi].label,
                                                     launcher_search)) {
                        if (cnt == menu_selected) { actual = fi; break; }
                        cnt++;
                    }
                }
                if (actual >= 0) {
                    void (*action)(void) = launcher_items[actual].action;
                    close_start_menu();
                    gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + TASKBAR_H + 8);
                    if (action) action();
                }
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_CHAR && key == 27) {
            close_start_menu();
            return 1;
        }

        /* Search: typed characters filter the app list */
        if (event_type == INPUT_EVENT_CHAR && key >= 32 && key < 127) {
            if (launcher_search_len < 31) {
                launcher_search[launcher_search_len++] = key;
                launcher_search[launcher_search_len] = '\0';
                menu_selected = 0;
                gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_BACKSPACE) {
            if (launcher_search_len > 0) {
                launcher_search[--launcher_search_len] = '\0';
                menu_selected = 0;
                gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            }
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
