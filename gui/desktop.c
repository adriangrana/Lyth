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
#include "renderer.h"
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
#include "wallpaper_default_png.h"
#include "icons/icon_lyth.h"
#include "icons/icon_terminal.h"
#include "icons/icon_nexus.h"
#include "icons/icon_calc.h"
#include "icons/icon_settings.h"
#include "icons/icon_notes.h"
/* Multi-resolution icons: 16px (micro UI), 24px (taskbar), 64px (launcher) */
#include "icons/icon_lyth_16.h"
#include "icons/icon_terminal_16.h"
#include "icons/icon_nexus_16.h"
#include "icons/icon_calc_16.h"
#include "icons/icon_settings_16.h"
#include "icons/icon_notes_16.h"
#include "icons/icon_lyth_24.h"
#include "icons/icon_terminal_24.h"
#include "icons/icon_nexus_24.h"
#include "icons/icon_calc_24.h"
#include "icons/icon_settings_24.h"
#include "icons/icon_notes_24.h"
#include "icons/icon_lyth_64.h"
#include "icons/icon_terminal_64.h"
#include "icons/icon_nexus_64.h"
#include "icons/icon_calc_64.h"
#include "icons/icon_settings_64.h"
#include "icons/icon_notes_64.h"

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

/* ---- colours: all sourced from theme runtime ---- */
#define COL_BG_TOP       theme.wall_top
#define COL_BG_MID       theme.wall_mid
#define COL_BG_BOT       theme.wall_bot

#define COL_TASKBAR_BG   theme.taskbar_bg
#define COL_TASKBAR_SEP  THEME_COL_TASKBAR_SEP
#define COL_TASKBAR_TEXT theme.text
#define COL_TASKBAR_DIM  theme.dim

#define COL_DOCK_BG      theme.dock_bg
#define COL_DOCK_HOVER   THEME_COL_DOCK_HOVER
#define COL_DOCK_ACTIVE  theme.accent
#define COL_DOCK_DOT     theme.accent

#define COL_TRAY_TEXT    theme.text
#define COL_TRAY_DIM     theme.dim

/* Launcher (app menu) colours */
#define COL_LAUNCH_BG    theme.launcher_bg
#define COL_LAUNCH_PANEL theme.mantle
#define COL_LAUNCH_SEARCH_BG theme.surface0
#define COL_LAUNCH_TEXT  theme.text
#define COL_LAUNCH_DIM   theme.dim
#define COL_LAUNCH_HOVER theme.accent
#define COL_LAUNCH_SEP   theme.border
#define COL_LAUNCH_ICON_BG theme.surface0
#define COL_LAUNCH_FOLDER  theme.dock_bg

/* Context menu */
#define COL_CTX_BG       theme.popup_bg
#define COL_CTX_HOVER    theme.accent
#define COL_CTX_TEXT     theme.text
#define COL_CTX_BORDER   theme.popup_border

/* Network / control panel */
#define COL_POPUP_BG     theme.popup_bg
#define COL_POPUP_BORDER theme.popup_border
#define COL_POPUP_TEXT   theme.text
#define COL_POPUP_DIM    theme.dim
#define COL_POPUP_BTN    theme.accent

#define COL_NET_GREEN    THEME_COL_SUCCESS
#define COL_NET_RED      THEME_COL_ERROR

/* Running app label */
#define COL_APP_LABEL_BG theme.dock_bg
#define COL_APP_LABEL_FG theme.text

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
#define DOCK_H           GUI_DOCK_HEIGHT
#define DOCK_ICON_SIZE   32
#define DOCK_ICON_PAD    10
#define DOCK_MAX_ITEMS   12
#define DOCK_Y_PAD       10

/* ---- system tray dimensions ---- */
#define TRAY_ICON_W      22
#define TRAY_PAD         8
#define CLOCK_DATE_W     120

/* ---- dock item ---- */
typedef struct {
    const char* label;
    void (*action)(void);
    uint32_t icon_color;
    char shortcut;           /* first letter for icon (fallback) */
    const uint32_t *icon_pixels;      /* 32x32 ARGB (dock native) */
    const uint32_t *icon_pixels_24;   /* 24x24 ARGB (taskbar native) */
    const uint32_t *icon_pixels_64;   /* 64x64 ARGB (hover/large) */
} dock_item_t;

/* ---- start menu / app launcher ---- */
#define LAUNCHER_W      480
#define LAUNCHER_H      560
#define LAUNCHER_COLS   5
#define LAUNCHER_ICON_SZ 48
#define LAUNCHER_ICON_PAD 12
#define LAUNCHER_CELL_W  ((LAUNCHER_W - 40) / LAUNCHER_COLS)
#define LAUNCHER_CELL_H  72
#define LAUNCHER_MAX     20

/* App categories */
#define CAT_CORE   0   /* Core apps */
#define CAT_UTIL   1   /* Utilities */
#define CAT_SYS    2   /* System */

static const char *cat_names[] = { "Core", "Utilities", "System" };

typedef struct {
    char label[24];
    void (*action)(void);
    uint32_t icon_color;
    char icon_letter;
    const uint32_t *icon_pixels;      /* 32x32 ARGB (dock/sidebar) */
    const uint32_t *icon_pixels_24;   /* 24x24 ARGB (taskbar native) */
    const uint32_t *icon_pixels_64;   /* 64x64 ARGB (launcher native) */
    int pinned;               /* 1 = show in pinned row */
    int category;             /* CAT_CORE / CAT_UTIL / CAT_SYS */
} launcher_item_t;

/* ---- recent apps tracking ---- */
#define RECENT_MAX  3
static int recent_apps[RECENT_MAX]; /* indices into launcher_items[] */
static int recent_count;

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

/* ---- power dialog ---- */
#define PWR_DLG_W  320
#define PWR_DLG_H  160
#define PWR_BTN_W  90
#define PWR_BTN_H  28
static int power_dlg_open;
static int power_dlg_x, power_dlg_y;
/* ---- launcher search ---- */
static char launcher_search[32];
static int launcher_search_len;

/* ---- tray click regions (set during render) ---- */
static int tray_net_x, tray_menu_x;

/* ---- dock/taskbar hover ---- */
static int dock_hover_idx = -1;  /* -1 = none */

/* ---- dock hover scale animation ---- */
#define DOCK_SCALE_MAX  6   /* max extra pixels per axis on hover */
#define DOCK_SCALE_STEPS 4  /* animation frames (0→DOCK_SCALE_MAX) */
static int dock_scale = 0;  /* current extra size (0..DOCK_SCALE_MAX) */

/* ---- dock drag-reorder ---- */
#define DOCK_DRAG_THRESHOLD  8
static int dock_drag_active;         /* 1 while reordering */
static int dock_drag_idx;            /* item being dragged */
static int dock_drag_start_mx;       /* mouse X at press */

/* ---- desktop shortcut icons ---- */
#define DICON_SZ    48   /* icon render size */
#define DICON_CELL  80   /* cell size (icon + label + padding) */
#define DICON_PAD   20   /* margin from screen edge */
#define DICON_MAX    8

typedef struct {
    const char* label;
    void (*action)(void);
    const uint32_t *icon_pixels;   /* 32x32 source ARGB */
    const uint32_t *icon_pixels_64; /* 64x64 source ARGB (preferred) */
    uint32_t icon_color;
    char icon_letter;
} dicon_item_t;

static dicon_item_t dicon_items[DICON_MAX];
static int dicon_count;
static int dicon_hover_idx = -1;

/* ---- launcher animation ---- */
#define LAUNCHER_ANIM_STEPS  6   /* frames for slide-up/down */
static int launcher_anim = 0;   /* 0=idle, >0=opening, <0=closing */
static int launcher_anim_step = 0;

/* ---- taskbar auto-hide ---- */
#define TB_AUTOHIDE_STEPS  4   /* animation frames (slide up/down) */
#define TB_HOTZONE  4          /* mouse y threshold to reveal taskbar */
static int tb_autohide = 0;   /* 0=always visible, 1=auto-hide enabled */
static int tb_visible = 1;    /* 1=shown, 0=hidden */
static int tb_anim = 0;       /* 0=idle, 1=showing, -1=hiding */
static int tb_anim_step = TB_AUTOHIDE_STEPS; /* 0=hidden, STEPS=shown */

/* ---- launcher cache surface (avoids full alpha-blend on every frame) ---- */
static gui_surface_t launcher_cache;
static int launcher_cache_valid;

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

static void wallpaper_extract_accent(void);  /* forward decl */

static void wallpaper_catalogue_init(void) {
    int i;
    if (wp_count > 0) return;
    /* Image wallpapers from defaults_wallpapers/ */

    wp_catalogue[wp_count].name = "Default";
    wp_catalogue[wp_count].type = WP_TYPE_IMAGE;
    wp_catalogue[wp_count].png_data = wallpaper_default_png_data;
    wp_catalogue[wp_count].png_size = (unsigned int)wallpaper_default_png_size;
    wp_count++;

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
    wallpaper_extract_accent();
}

/* Extract a dominant saturated colour from the current wallpaper.
 * Samples a grid of pixels, accumulates into 6 hue buckets (R,Y,G,C,B,M),
 * weighted by saturation. Returns the colour of the strongest bucket.
 * Returns 0 if no wallpaper or not enough saturation. */
static uint32_t wp_suggest_accent;   /* cached result */

static void wallpaper_extract_accent(void)
{
    unsigned int buckets[6] = {0,0,0,0,0,0};  /* hue bins */
    unsigned int bucket_r[6]={0,0,0,0,0,0}, bucket_g[6]={0,0,0,0,0,0}, bucket_b[6]={0,0,0,0,0,0};
    unsigned int bucket_n[6]={0,0,0,0,0,0};
    int iw, ih, step_x, step_y, x, y, best;
    unsigned int best_weight;

    wp_suggest_accent = 0;
    if (!wallpaper_img.pixels || wallpaper_img.width < 2 || wallpaper_img.height < 2)
        return;

    iw = wallpaper_img.width;
    ih = wallpaper_img.height;
    /* sample ~400 pixels in a grid */
    step_x = iw / 20; if (step_x < 1) step_x = 1;
    step_y = ih / 20; if (step_y < 1) step_y = 1;

    for (y = step_y / 2; y < ih; y += step_y) {
        for (x = step_x / 2; x < iw; x += step_x) {
            uint32_t px = wallpaper_img.pixels[y * iw + x];
            int r = (px >> 16) & 0xFF, g = (px >> 8) & 0xFF, b = px & 0xFF;
            int mx = r; if (g > mx) mx = g; if (b > mx) mx = b;
            int mn = r; if (g < mn) mn = g; if (b < mn) mn = b;
            int sat = mx - mn;
            int hue_bin;
            if (sat < 30 || mx < 40) continue;  /* skip grey/dark pixels */

            /* Approximate hue bucket (0-5) */
            if (mx == r) {
                if (g >= b) hue_bin = 0;      /* red/yellow */
                else        hue_bin = 5;      /* magenta */
            } else if (mx == g) {
                if (r >= b) hue_bin = 1;      /* yellow/green */
                else        hue_bin = 2;      /* green/cyan */
            } else {
                if (g >= r) hue_bin = 3;      /* cyan/blue */
                else        hue_bin = 4;      /* blue/magenta */
            }
            buckets[hue_bin] += (unsigned int)sat;
            bucket_r[hue_bin] += (unsigned int)r;
            bucket_g[hue_bin] += (unsigned int)g;
            bucket_b[hue_bin] += (unsigned int)b;
            bucket_n[hue_bin]++;
        }
    }

    /* Find best bucket */
    best = 0; best_weight = buckets[0];
    for (x = 1; x < 6; x++) {
        if (buckets[x] > best_weight) { best = x; best_weight = buckets[x]; }
    }
    if (bucket_n[best] == 0 || best_weight < 500) return;

    /* Average colour of best bucket */
    {
        unsigned int n = bucket_n[best];
        int ar = (int)(bucket_r[best] / n);
        int ag = (int)(bucket_g[best] / n);
        int ab = (int)(bucket_b[best] / n);
        /* Boost saturation to make it a good accent */
        int max_c = ar; if (ag > max_c) max_c = ag; if (ab > max_c) max_c = ab;
        if (max_c > 0 && max_c < 200) {
            int scale = 200 * 256 / max_c;
            ar = ar * scale / 256; if (ar > 255) ar = 255;
            ag = ag * scale / 256; if (ag > 255) ag = 255;
            ab = ab * scale / 256; if (ab > 255) ab = 255;
        }
        wp_suggest_accent = ((uint32_t)ar << 16) | ((uint32_t)ag << 8) | (uint32_t)ab;
    }
}

uint32_t desktop_get_wp_accent(void)
{
    return wp_suggest_accent;
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

/* Blit a pre-decoded ARGB icon onto a surface with alpha blending */
static void blit_icon(gui_surface_t *dst, int dx, int dy,
                      const uint32_t *pixels, int iw, int ih) {
    int row_i, col_i;
    for (row_i = 0; row_i < ih; row_i++) {
        int py = dy + row_i;
        if (py < 0 || py >= dst->height) continue;
        for (col_i = 0; col_i < iw; col_i++) {
            int px = dx + col_i;
            if (px < 0 || px >= dst->width) continue;
            uint32_t p = pixels[row_i * iw + col_i];
            int alpha = (int)((p >> 24) & 0xFF);
            if (alpha == 0) continue;
            if (alpha == 255) {
                dst->pixels[py * dst->stride + px] = p & 0x00FFFFFF;
            } else {
                uint32_t bg = dst->pixels[py * dst->stride + px];
                int ia = 255 - alpha;
                int r = ((int)((p >> 16) & 0xFF) * alpha +
                         (int)((bg >> 16) & 0xFF) * ia) / 255;
                int g = ((int)((p >> 8) & 0xFF) * alpha +
                         (int)((bg >> 8) & 0xFF) * ia) / 255;
                int b = ((int)(p & 0xFF) * alpha +
                         (int)(bg & 0xFF) * ia) / 255;
                dst->pixels[py * dst->stride + px] =
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}

/* Blit a pre-decoded ARGB icon scaled to tw x th (bilinear + alpha) */
static void blit_icon_scaled(gui_surface_t *dst, int dx, int dy,
                             const uint32_t *pixels, int iw, int ih,
                             int tw, int th) {
    int row_i, col_i;
    if (tw <= 0 || th <= 0 || iw <= 0 || ih <= 0) return;
    for (row_i = 0; row_i < th; row_i++) {
        int py = dy + row_i;
        if (py < 0 || py >= dst->height) continue;
        int fy16 = row_i * ((ih - 1) << 16) / (th > 1 ? th - 1 : 1);
        int y0 = fy16 >> 16, y1 = y0 + 1;
        int fy = fy16 & 0xFFFF, ify = 0x10000 - fy;
        if (y1 >= ih) y1 = ih - 1;
        for (col_i = 0; col_i < tw; col_i++) {
            int px = dx + col_i;
            if (px < 0 || px >= dst->width) continue;
            int fx16 = col_i * ((iw - 1) << 16) / (tw > 1 ? tw - 1 : 1);
            int x0 = fx16 >> 16, x1 = x0 + 1;
            int fx = fx16 & 0xFFFF, ifx = 0x10000 - fx;
            if (x1 >= iw) x1 = iw - 1;
            uint32_t c00 = pixels[y0 * iw + x0];
            uint32_t c10 = pixels[y0 * iw + x1];
            uint32_t c01 = pixels[y1 * iw + x0];
            uint32_t c11 = pixels[y1 * iw + x1];
            int a = (int)((((c00>>24)&0xFF)*ifx+((c10>>24)&0xFF)*fx)>>16)*ify
                  + (int)((((c01>>24)&0xFF)*ifx+((c11>>24)&0xFF)*fx)>>16)*fy;
            a >>= 16; if (a > 255) a = 255;
            if (a == 0) continue;
            int r = (int)((((c00>>16)&0xFF)*ifx+((c10>>16)&0xFF)*fx)>>16)*ify
                  + (int)((((c01>>16)&0xFF)*ifx+((c11>>16)&0xFF)*fx)>>16)*fy;
            int g = (int)((((c00>>8)&0xFF)*ifx+((c10>>8)&0xFF)*fx)>>16)*ify
                  + (int)((((c01>>8)&0xFF)*ifx+((c11>>8)&0xFF)*fx)>>16)*fy;
            int b = (int)(((c00&0xFF)*ifx+(c10&0xFF)*fx)>>16)*ify
                  + (int)(((c01&0xFF)*ifx+(c11&0xFF)*fx)>>16)*fy;
            r >>= 16; g >>= 16; b >>= 16;
            if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
            if (a == 255) {
                dst->pixels[py * dst->stride + px] =
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            } else {
                uint32_t bg = dst->pixels[py * dst->stride + px];
                int ia = 255 - a;
                r = (r * a + (int)((bg >> 16) & 0xFF) * ia) / 255;
                g = (g * a + (int)((bg >> 8) & 0xFF) * ia) / 255;
                b = (b * a + (int)(bg & 0xFF) * ia) / 255;
                dst->pixels[py * dst->stride + px] =
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
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

/* In-place 5x5 box blur on a rectangular region of a surface.
 * Reads from src pixels (original) and writes into dst.
 * Uses a simple kernel average — fast enough for small UI regions. */
static void box_blur_region(gui_surface_t *dst, int x0, int y0, int w, int h) {
    int row, col, ky, kx;
    int sx = dst->stride;
    int dw = dst->width, dh = dst->height;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > dw) w = dw - x0;
    if (y0 + h > dh) h = dh - y0;
    if (w <= 0 || h <= 0) return;

    /* Two-pass: horizontal then vertical would be faster, but for small
     * UI regions a direct 5x5 read is acceptable. We read directly from
     * the surface and do 2 passes to converge. */
    for (int pass = 0; pass < 2; pass++) {
        for (row = y0; row < y0 + h; row++) {
            for (col = x0; col < x0 + w; col++) {
                unsigned int sr = 0, sg = 0, sb = 0, cnt = 0;
                for (ky = -2; ky <= 2; ky++) {
                    int py = row + ky;
                    if (py < 0 || py >= dh) continue;
                    for (kx = -2; kx <= 2; kx++) {
                        int px = col + kx;
                        if (px < 0 || px >= dw) continue;
                        uint32_t p = dst->pixels[py * sx + px];
                        sr += (p >> 16) & 0xFF;
                        sg += (p >> 8) & 0xFF;
                        sb += p & 0xFF;
                        cnt++;
                    }
                }
                if (cnt > 0)
                    dst->pixels[row * sx + col] = ((sr/cnt) << 16) |
                                                   ((sg/cnt) << 8) |
                                                   (sb/cnt);
            }
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

/* Alpha-blended character drawing (per-glyph-pixel blend) */
static void draw_char_alpha(gui_surface_t* s, int x, int y, unsigned char ch,
                             uint32_t col, int alpha) {
    int row_i, col_i;
    uint8_t bits;
    uint32_t fr = (col >> 16) & 0xFF;
    uint32_t fg = (col >> 8) & 0xFF;
    uint32_t fb_c = col & 0xFF;
    int ia = 255 - alpha;
    if (!s || !s->pixels) return;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    if (x + FONT_PSF_WIDTH <= 0 || x >= s->width ||
        y + FONT_PSF_HEIGHT <= 0 || y >= s->height) return;
    for (row_i = 0; row_i < FONT_PSF_HEIGHT; row_i++) {
        int py = y + row_i;
        if (py < 0 || py >= s->height) continue;
        bits = font_psf_data[ch][row_i];
        uint32_t* dst = &s->pixels[py * s->stride];
        for (col_i = 0; col_i < FONT_PSF_WIDTH; col_i++) {
            int px = x + col_i;
            if (px < 0 || px >= s->width) continue;
            if (bits & (0x80u >> col_i)) {
                uint32_t bg = dst[px];
                uint32_t r = (fr * (uint32_t)alpha + ((bg >> 16) & 0xFF) * (uint32_t)ia) / 255;
                uint32_t g = (fg * (uint32_t)alpha + ((bg >> 8) & 0xFF) * (uint32_t)ia) / 255;
                uint32_t b = (fb_c * (uint32_t)alpha + (bg & 0xFF) * (uint32_t)ia) / 255;
                dst[px] = (r << 16) | (g << 8) | b;
            }
        }
    }
}

static void draw_string_alpha(gui_surface_t* s, int x, int y,
                               const char* str, uint32_t col, int alpha) {
    if (!str) return;
    while (*str) {
        draw_char_alpha(s, x, y, (unsigned char)*str, col, alpha);
        x += FONT_PSF_WIDTH;
        str++;
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

/* forward declaration */
static void toggle_start_menu(void);

/* Track recently launched apps (push idx to front, shift others) */
static void recent_track(int idx) {
    int i, j;
    /* Remove if already in list */
    for (i = 0; i < recent_count; i++) {
        if (recent_apps[i] == idx) {
            for (j = i; j > 0; j--) recent_apps[j] = recent_apps[j - 1];
            recent_apps[0] = idx;
            return;
        }
    }
    /* Insert at front, shift rest */
    if (recent_count < RECENT_MAX) recent_count++;
    for (j = recent_count - 1; j > 0; j--) recent_apps[j] = recent_apps[j - 1];
    recent_apps[0] = idx;
}

/* ---- Get dock metrics ---- */
static int dock_total_w(void) {
    return dock_item_count * (DOCK_ICON_SIZE + DOCK_ICON_PAD) + DOCK_ICON_PAD;
}
static int dock_start_x(void) {
    return (sw - dock_total_w()) / 2;
}

static void rebuild_desktop(void) {
    int y, taskbar_y, dock_y;
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
            int fy16 = (int)((long)(y + sy_off) * scale_den * 65536 / scale_num);
            uint32_t *dst_row;
            dst_row = &desk_surf.pixels[y * sw];
            for (int x = 0; x < sw; x++) {
                int fx16 = (int)((long)(x + sx_off) * scale_den * 65536 / scale_num);
                dst_row[x] = bilinear_sample_wp(wallpaper_img.pixels,
                                                 iw, ih, fx16, fy16);
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
            for (y = half; y < sh; y++) {
                uint32_t c = mix_rgb(COL_BG_MID, COL_BG_BOT, y - half,
                                     (sh - half) > 1 ? (sh - half - 1) : 1);
                memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
            }
        }
    }

    /* Subtle dark scrim for text legibility over bright wallpapers */
    alpha_blend_fill(&desk_surf, 0, TASKBAR_H, sw, sh - TASKBAR_H - DOCK_H, 0x000000, 18);

    /* Subtle centered watermark (visible on empty desktop) */
    {
        const char *wm = "Lyth OS";
        int wm_len = 7;
        int wm_w = wm_len * FONT_PSF_WIDTH;
        int wm_x = (sw - wm_w) / 2;
        int wm_y = TASKBAR_H + (sh - TASKBAR_H - DOCK_H) * 2 / 5;
        draw_string_alpha(&desk_surf, wm_x, wm_y, wm, THEME_COL_TEXT, 25);
    }

    /* ---- Desktop shortcut icons (upper-right grid, top-to-bottom) ---- */
    {
        int di;
        int base_x = sw - DICON_PAD - DICON_CELL;
        int base_y = TASKBAR_H + DICON_PAD;
        for (di = 0; di < dicon_count; di++) {
            int col = di / ((sh - TASKBAR_H - DOCK_H - 2 * DICON_PAD) / DICON_CELL);
            int row = di % ((sh - TASKBAR_H - DOCK_H - 2 * DICON_PAD) / DICON_CELL);
            int cx = base_x - col * DICON_CELL;
            int cy = base_y + row * DICON_CELL;
            int icon_x = cx + (DICON_CELL - DICON_SZ) / 2;
            int icon_y = cy;

            /* Draw icon */
            if (dicon_items[di].icon_pixels_64) {
                blit_icon_scaled(&desk_surf, icon_x, icon_y,
                                 dicon_items[di].icon_pixels_64, 64, 64,
                                 DICON_SZ, DICON_SZ);
            } else if (dicon_items[di].icon_pixels) {
                blit_icon_scaled(&desk_surf, icon_x, icon_y,
                                 dicon_items[di].icon_pixels, 32, 32,
                                 DICON_SZ, DICON_SZ);
            } else {
                /* Fallback: colored square + letter */
                alpha_blend_fill(&desk_surf, icon_x, icon_y,
                                 DICON_SZ, DICON_SZ,
                                 dicon_items[di].icon_color, 200);
                draw_char_alpha(&desk_surf,
                    icon_x + (DICON_SZ - FONT_PSF_WIDTH) / 2,
                    icon_y + (DICON_SZ - FONT_PSF_HEIGHT) / 2,
                    (unsigned char)dicon_items[di].icon_letter,
                    THEME_COL_TEXT, 220);
            }

            /* Label centered below icon */
            {
                const char *lbl = dicon_items[di].label;
                int llen = str_length(lbl);
                int lw = llen * FONT_PSF_WIDTH;
                int lx = cx + (DICON_CELL - lw) / 2;
                int ly = icon_y + DICON_SZ + 4;
                draw_string_alpha(&desk_surf, lx, ly, lbl, THEME_COL_TEXT, 180);
            }
        }
    }

    /* ---- Top Taskbar (running apps + tray) ---- */
    {
    int tb_off = 0;
    if (tb_autohide && tb_anim_step < TB_AUTOHIDE_STEPS)
        tb_off = TASKBAR_H * (TB_AUTOHIDE_STEPS - tb_anim_step) / TB_AUTOHIDE_STEPS;
    taskbar_y = -tb_off;

    /* Frosted glass blur behind taskbar */
    if (taskbar_y + TASKBAR_H > 0)
        box_blur_region(&desk_surf, 0, taskbar_y > 0 ? taskbar_y : 0, sw,
                        taskbar_y + TASKBAR_H);

    /* Taskbar background — translucent glass over wallpaper */
    alpha_blend_fill(&desk_surf, 0, taskbar_y, sw, TASKBAR_H, 0x0D1117, 190);
    /* Subtle accent line at bottom edge */
    alpha_blend_fill(&desk_surf, 0, taskbar_y + TASKBAR_H - 1, sw, 1, THEME_COL_ACCENT, 30);

    /* == Left section: running app icons (non-pinned apps only) == */
    #define TB_ICON_SZ 24
    #define TB_ICON_PAD 6
    {
        int lx = 8;
        int i, count = gui_window_count();
        for (i = 0; i < count; i++) {
            gui_window_t* w = gui_window_get(i);
            if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
            if (!gui_window_on_current_ws(w)) continue;
            /* Skip apps that are pinned in the dock */
            {
                int pinned = 0, d;
                for (d = 0; d < dock_item_count; d++) {
                    if (dock_items[d].label &&
                        str_starts_with(w->title, dock_items[d].label)) {
                        pinned = 1; break;
                    }
                }
                if (pinned) continue;
            }
            int minimized = (w->flags & GUI_WIN_MINIMIZED) ? 1 : 0;

            /* Find icon for this window (prefer native 24px for taskbar) */
            const uint32_t *win_icon_24 = 0;
            const uint32_t *win_icon_32 = 0;
            uint32_t win_icon_col = COL_APP_LABEL_BG;
            char win_icon_letter = '?';
            {
                int li;
                for (li = 0; li < launcher_item_count; li++) {
                    if (str_starts_with(w->title, launcher_items[li].label)) {
                        win_icon_24 = launcher_items[li].icon_pixels_24;
                        win_icon_32 = launcher_items[li].icon_pixels;
                        win_icon_col = launcher_items[li].icon_color;
                        win_icon_letter = launcher_items[li].icon_letter;
                        break;
                    }
                }
            }

            int item_w = TB_ICON_SZ + TB_ICON_PAD * 2;
            if (lx + item_w > sw / 2) break;

            int iy = taskbar_y + (TASKBAR_H - TB_ICON_SZ) / 2;
            /* Background pill */
            draw_rounded_rect(&desk_surf, lx, taskbar_y + 4, item_w,
                              TASKBAR_H - 8, THEME_RADIUS_PILL,
                              minimized ? THEME_COL_SURFACE0 : COL_APP_LABEL_BG);
            /* Icon or fallback letter */
            if (win_icon_24) {
                /* Native 24x24 icon — direct blit, no scaling */
                blit_icon(&desk_surf, lx + TB_ICON_PAD, iy,
                          win_icon_24, 24, 24);
            } else if (win_icon_32) {
                /* Fallback: scale 32x32 → 24x24 */
                blit_icon_scaled(&desk_surf, lx + TB_ICON_PAD, iy,
                                 win_icon_32, 32, 32, TB_ICON_SZ, TB_ICON_SZ);
            } else {
                /* Fallback: colored square with letter */
                int sq = TB_ICON_SZ - 4;
                int sqx = lx + TB_ICON_PAD + 2;
                int sqy = iy + 2;
                draw_rounded_rect(&desk_surf, sqx, sqy, sq, sq, THEME_RADIUS_MD, win_icon_col);
                gui_surface_draw_char(&desk_surf,
                    sqx + (sq - FONT_PSF_WIDTH) / 2,
                    sqy + (sq - FONT_PSF_HEIGHT) / 2,
                    win_icon_letter, COL_TASKBAR_TEXT, 0, 0);
            }
            /* Underline indicator for active (non-minimized) */
            if (!minimized) {
                alpha_blend_fill(&desk_surf, lx + 4, taskbar_y + TASKBAR_H - 3,
                                 item_w - 8, 2, THEME_COL_ACCENT, 180);
            }
            lx += item_w + 4;
        }
    }

    /* == Center: workspace indicators == */
    {
        int ws_pill_w = 18, ws_gap = 4;
        int ws_total = GUI_MAX_WORKSPACES * ws_pill_w + (GUI_MAX_WORKSPACES - 1) * ws_gap;
        int ws_x = (sw - ws_total) / 2;
        int ws_y = taskbar_y + (TASKBAR_H - 10) / 2;
        int wsi;
        for (wsi = 0; wsi < GUI_MAX_WORKSPACES; wsi++) {
            int active = (wsi == gui_workspace_current());
            uint32_t pill_col = active ? THEME_COL_ACCENT : 0x3A3D4A;
            int pill_alpha = active ? 255 : 120;
            alpha_blend_fill(&desk_surf, ws_x, ws_y, ws_pill_w, 10,
                             pill_col, pill_alpha);
            /* dot or number */
            if (active) {
                gui_surface_fill(&desk_surf, ws_x + ws_pill_w / 2 - 1,
                                 ws_y + 3, 3, 3, 0xFFFFFF);
            } else {
                gui_surface_fill(&desk_surf, ws_x + ws_pill_w / 2,
                                 ws_y + 4, 2, 2, 0x888888);
            }
            ws_x += ws_pill_w + ws_gap;
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
    } /* end taskbar auto-hide offset scope */

    /* ---- Bottom Dock (Lyth icon + pinned app icons) ---- */
    dock_y = sh - DOCK_H;

    /* == Center: dock icons == */
    {
        int dx = dock_start_x();
        int dy = dock_y + DOCK_Y_PAD;
        int pill_x = dx - 8;
        int pill_w = dock_total_w() + 16;
        int i;

        /* Frosted glass blur behind dock pill */
        box_blur_region(&desk_surf, pill_x, dock_y + 2, pill_w, DOCK_H - 4);

        /* Dock background pill — translucent */
        draw_rounded_rect_alpha(&desk_surf, pill_x, dock_y + 2,
                                pill_w, DOCK_H - 4, THEME_RADIUS_PILL, 0x0D1117, 200);
        /* Subtle accent border at top of pill */
        alpha_blend_fill(&desk_surf, pill_x, dock_y + 2, pill_w, 1, THEME_COL_ACCENT, 30);

        for (i = 0; i < dock_item_count; i++) {
            int ix = dx + DOCK_ICON_PAD + i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
            int iy = dy;

            if (dock_items[i].icon_pixels) {
                /* Blit pre-decoded ARGB icon */
                blit_icon(&desk_surf, ix, iy, dock_items[i].icon_pixels, 32, 32);
            } else {
                /* Fallback: colored rounded square with letter */
                draw_rounded_rect(&desk_surf, ix, iy, DOCK_ICON_SIZE,
                                  DOCK_ICON_SIZE, THEME_RADIUS_MD, dock_items[i].icon_color);
                gui_surface_draw_char(&desk_surf,
                    ix + (DOCK_ICON_SIZE - FONT_PSF_WIDTH) / 2,
                    iy + (DOCK_ICON_SIZE - FONT_PSF_HEIGHT) / 2,
                    (unsigned char)dock_items[i].shortcut,
                    COL_TASKBAR_TEXT, 0, 0);
            }

            /* Running indicator dot below icon (skip Lyth/menu item) */
            if (dock_items[i].label && dock_items[i].action != toggle_start_menu) {
                int j, wcount = gui_window_count();
                for (j = 0; j < wcount; j++) {
                    gui_window_t* w = gui_window_get(j);
                    if (w && (w->flags & GUI_WIN_VISIBLE) &&
                        gui_window_on_current_ws(w) &&
                        str_starts_with(w->title, dock_items[i].label)) {
                        gui_surface_fill(&desk_surf,
                            ix + DOCK_ICON_SIZE / 2 - 3,
                            dock_y + DOCK_H - 5,
                            6, 2, COL_DOCK_DOT);
                        break;
                    }
                }
            }
        }
    }

    desk_valid = 1;
}

/* ---- Launcher cache management ---- */

static void launcher_cache_rebuild(void) {
    int lx = (sw - LAUNCHER_W) / 2;
    int ly = sh - DOCK_H - LAUNCHER_H - 8;
    int ar;

    /* Allocate on first use */
    if (!launcher_cache.pixels) {
        gui_surface_alloc(&launcher_cache, LAUNCHER_W, LAUNCHER_H);
        if (!launcher_cache.pixels) return;
    }

    /* Copy wallpaper region as base */
    if (desk_surf.pixels) {
        for (ar = 0; ar < LAUNCHER_H; ar++) {
            int sy = ly + ar;
            if (sy >= 0 && sy < sh) {
                int sx0 = lx < 0 ? 0 : lx;
                int sx1 = lx + LAUNCHER_W > sw ? sw : lx + LAUNCHER_W;
                int dx = sx0 - lx;
                if (sx1 > sx0)
                    memcpy(&launcher_cache.pixels[ar * LAUNCHER_W + dx],
                           &desk_surf.pixels[sy * sw + sx0],
                           (size_t)(sx1 - sx0) * 4);
            }
        }
    }

    launcher_cache_valid = 1;
}

static void draw_start_menu(void);

static void launcher_cache_invalidate(void) {
    launcher_cache_valid = 0;
}

/* Fully rebuild the launcher cache: wallpaper base + launcher drawn on top */
static void launcher_cache_render(void) {
    launcher_cache_rebuild();
    if (!launcher_cache.pixels) return;
    /* Frosted glass blur behind launcher */
    box_blur_region(&launcher_cache, 0, 0, LAUNCHER_W, LAUNCHER_H);
    draw_start_menu();
    /* launcher_cache_valid already set by rebuild; keep it */
}

/* draw_start_menu: renders launcher content into the cache surface
 * (LAUNCHER_W × LAUNCHER_H, origin at 0,0).  Call launcher_cache_rebuild()
 * first to seed the wallpaper background. */
static void draw_start_menu(void) {
    gui_surface_t *dst = &launcher_cache;
    /* local offsets (0-based) */
    int lx = 0, ly = 0;
    int i, row, col;
    int grid_y;

    /* Build filtered list based on search */
    int filtered[LAUNCHER_MAX];
    int fcount = 0;
    for (i = 0; i < launcher_item_count; i++) {
        if (launcher_search_len == 0 ||
            str_starts_with_ignore_case(launcher_items[i].label, launcher_search))
            filtered[fcount++] = i;
    }

    /* Background panel — translucent glass */
    draw_rounded_rect_alpha(dst, lx, ly, LAUNCHER_W, LAUNCHER_H, THEME_RADIUS_LG, COL_LAUNCH_BG, 210);
    /* Subtle accent border at top */
    alpha_blend_fill(dst, lx + 3, ly, LAUNCHER_W - 6, 1, THEME_COL_ACCENT, 25);

    /* Top: OS title + search bar */
    gui_surface_draw_string(dst, lx + 20, ly + 16, "Lyth OS", THEME_COL_ACCENT, 0, 0);

    /* Search bar */
    {
        int sx = lx + 100, sy = ly + 12, swidth = LAUNCHER_W - 140, sheight = 24;
        draw_rounded_rect(dst, sx, sy, swidth, sheight, THEME_RADIUS_SM, COL_LAUNCH_SEARCH_BG);
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

    /* ---- Pinned apps row (only when not searching) ---- */
    grid_y = ly + 60;
    if (launcher_search_len == 0) {
        int pcol = 0;
        int pin_x = lx + 20;
        gui_surface_draw_string(dst, pin_x, ly + 54, "Pinned", COL_LAUNCH_DIM, 0, 0);
        grid_y = ly + 72;
        for (i = 0; i < launcher_item_count && pcol < LAUNCHER_COLS; i++) {
            if (!launcher_items[i].pinned) continue;
            {
                int cx = pin_x + pcol * LAUNCHER_CELL_W;
                int cy = grid_y;
                int icon_x = cx + (LAUNCHER_CELL_W - 4 - LAUNCHER_ICON_SZ) / 2;
                int icon_y = cy + 4;
                if (launcher_items[i].icon_pixels_64) {
                    blit_icon_scaled(dst, icon_x, icon_y,
                                     launcher_items[i].icon_pixels_64, 64, 64,
                                     LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
                } else if (launcher_items[i].icon_pixels) {
                    blit_icon_scaled(dst, icon_x, icon_y,
                                     launcher_items[i].icon_pixels, 32, 32,
                                     LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
                } else {
                    draw_rounded_rect(dst, icon_x, icon_y, LAUNCHER_ICON_SZ,
                                      LAUNCHER_ICON_SZ, THEME_RADIUS_MD, launcher_items[i].icon_color);
                    gui_surface_draw_char(dst,
                        icon_x + (LAUNCHER_ICON_SZ - FONT_PSF_WIDTH) / 2,
                        icon_y + (LAUNCHER_ICON_SZ - FONT_PSF_HEIGHT) / 2,
                        (unsigned char)launcher_items[i].icon_letter,
                        COL_LAUNCH_TEXT, 0, 0);
                }
                {
                    int lbl_w = (int)strlen(launcher_items[i].label) * FONT_PSF_WIDTH;
                    int lbl_x = cx + (LAUNCHER_CELL_W - 4 - lbl_w) / 2;
                    if (lbl_x < cx) lbl_x = cx;
                    gui_surface_draw_string_n(dst, lbl_x, cy + LAUNCHER_ICON_SZ + 10,
                        launcher_items[i].label,
                        (LAUNCHER_CELL_W - 4) / FONT_PSF_WIDTH,
                        COL_LAUNCH_TEXT, 0, 0);
                }
                pcol++;
            }
        }
        grid_y += LAUNCHER_CELL_H + 4;

        /* ---- Recent apps row ---- */
        if (recent_count > 0) {
            alpha_blend_fill(dst, lx + 16, grid_y - 8, LAUNCHER_W - 32, 1, COL_LAUNCH_SEP, 100);
            gui_surface_draw_string(dst, pin_x, grid_y - 4, "Recent", COL_LAUNCH_DIM, 0, 0);
            grid_y += 12;
            for (i = 0; i < recent_count; i++) {
                int ri = recent_apps[i];
                if (ri < 0 || ri >= launcher_item_count) continue;
                {
                    int cx = pin_x + i * LAUNCHER_CELL_W;
                    int cy = grid_y;
                    int icon_x = cx + (LAUNCHER_CELL_W - 4 - LAUNCHER_ICON_SZ) / 2;
                    int icon_y = cy + 4;
                    if (launcher_items[ri].icon_pixels_64) {
                        blit_icon_scaled(dst, icon_x, icon_y,
                                         launcher_items[ri].icon_pixels_64, 64, 64,
                                         LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
                    } else if (launcher_items[ri].icon_pixels) {
                        blit_icon_scaled(dst, icon_x, icon_y,
                                         launcher_items[ri].icon_pixels, 32, 32,
                                         LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
                    } else {
                        draw_rounded_rect(dst, icon_x, icon_y, LAUNCHER_ICON_SZ,
                                          LAUNCHER_ICON_SZ, THEME_RADIUS_MD, launcher_items[ri].icon_color);
                        gui_surface_draw_char(dst,
                            icon_x + (LAUNCHER_ICON_SZ - FONT_PSF_WIDTH) / 2,
                            icon_y + (LAUNCHER_ICON_SZ - FONT_PSF_HEIGHT) / 2,
                            (unsigned char)launcher_items[ri].icon_letter,
                            COL_LAUNCH_TEXT, 0, 0);
                    }
                    {
                        int lbl_w = (int)strlen(launcher_items[ri].label) * FONT_PSF_WIDTH;
                        int lbl_x = cx + (LAUNCHER_CELL_W - 4 - lbl_w) / 2;
                        if (lbl_x < cx) lbl_x = cx;
                        gui_surface_draw_string_n(dst, lbl_x, cy + LAUNCHER_ICON_SZ + 10,
                            launcher_items[ri].label,
                            (LAUNCHER_CELL_W - 4) / FONT_PSF_WIDTH,
                            COL_LAUNCH_TEXT, 0, 0);
                    }
                }
            }
            grid_y += LAUNCHER_CELL_H + 4;
        }

        /* Separator + "All Apps" label */
        alpha_blend_fill(dst, lx + 16, grid_y - 8, LAUNCHER_W - 32, 1, COL_LAUNCH_SEP, 100);
        gui_surface_draw_string(dst, pin_x, grid_y - 4, "All Apps", COL_LAUNCH_DIM, 0, 0);
        grid_y += 12;
    }

    /* App grid (filtered) */
    {
    int grid_x = lx + 20;
    for (i = 0; i < fcount; i++) {
        int idx = filtered[i];
        row = i / LAUNCHER_COLS;
        col = i % LAUNCHER_COLS;

        {
        int cx = grid_x + col * LAUNCHER_CELL_W;
        int cy = grid_y + row * LAUNCHER_CELL_H;

        /* Hover highlight for selected item */
        if (i == menu_selected) {
            draw_rounded_rect_alpha(dst, cx, cy, LAUNCHER_CELL_W - 4,
                              LAUNCHER_CELL_H - 4, THEME_RADIUS_SM, COL_LAUNCH_HOVER, 60);
        }

        /* Icon (colored rounded square or real icon) */
        {
            int icon_x = cx + (LAUNCHER_CELL_W - 4 - LAUNCHER_ICON_SZ) / 2;
            int icon_y = cy + 4;
            if (launcher_items[idx].icon_pixels_64) {
                blit_icon_scaled(dst, icon_x, icon_y,
                                 launcher_items[idx].icon_pixels_64, 64, 64,
                                 LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
            } else if (launcher_items[idx].icon_pixels) {
                blit_icon_scaled(dst, icon_x, icon_y,
                                 launcher_items[idx].icon_pixels, 32, 32,
                                 LAUNCHER_ICON_SZ, LAUNCHER_ICON_SZ);
            } else {
                draw_rounded_rect(dst, icon_x, icon_y, LAUNCHER_ICON_SZ,
                                  LAUNCHER_ICON_SZ, THEME_RADIUS_MD, launcher_items[idx].icon_color);
                gui_surface_draw_char(dst,
                    icon_x + (LAUNCHER_ICON_SZ - FONT_PSF_WIDTH) / 2,
                    icon_y + (LAUNCHER_ICON_SZ - FONT_PSF_HEIGHT) / 2,
                    (unsigned char)launcher_items[idx].icon_letter,
                    COL_LAUNCH_TEXT, 0, 0);
            }
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
                (i == menu_selected) ? theme_contrast_text(THEME_COL_ACCENT) : COL_LAUNCH_TEXT, 0, 0);
        }
        }
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
        draw_rounded_rect(dst, bx, by, 80, 24, THEME_RADIUS_SM, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 12, by + 4, "Logout", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Restart */
        draw_rounded_rect(dst, bx, by, 80, 24, THEME_RADIUS_SM, COL_LAUNCH_SEARCH_BG);
        gui_surface_draw_string(dst, bx + 8, by + 4, "Restart", COL_LAUNCH_TEXT, 0, 0);
        bx += 90;

        /* Shutdown */
        draw_rounded_rect(dst, bx, by, 90, 24, THEME_RADIUS_SM, THEME_COL_ERROR);
        gui_surface_draw_string(dst, bx + 6, by + 4, "Shutdown", theme_contrast_text(THEME_COL_ERROR), 0, 0);
    }
}

static void draw_context_menu(gui_surface_t* dst) {
    static const char* ctx_labels[CTX_MAX_ITEMS] = { "Close", "Minimize", "Maximize" };
    int i;
    int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;

    draw_rounded_rect_alpha(dst, ctx_menu_x, ctx_menu_y, CTX_W, total_h, THEME_RADIUS_MD, COL_CTX_BG, 220);
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
    int ly = sh - DOCK_H - LAUNCHER_H - 8;
    /* Start close animation — don't clear start_menu_open until anim done */
    launcher_anim = -1; /* closing */
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
    int py = sh - DOCK_H - NET_POPUP_H - 8;
    gui_dirty_add(px, py, NET_POPUP_W, NET_POPUP_H);
    net_popup_open = 0;
}

/* ---- Power dialog (Shutdown / Reboot / Cancel) ---- */
static void open_power_dialog(void) {
    if (power_dlg_open) return;
    power_dlg_x = (sw - PWR_DLG_W) / 2;
    power_dlg_y = (sh - PWR_DLG_H) / 2;
    power_dlg_open = 1;
    gui_dirty_add(power_dlg_x - 4, power_dlg_y - 4,
                  PWR_DLG_W + 8, PWR_DLG_H + 8);
}

static void close_power_dialog(void) {
    if (!power_dlg_open) return;
    gui_dirty_add(power_dlg_x - 4, power_dlg_y - 4,
                  PWR_DLG_W + 8, PWR_DLG_H + 8);
    power_dlg_open = 0;
}

static void draw_power_dialog(gui_surface_t* dst) {
    int dx = power_dlg_x, dy = power_dlg_y;
    int brow_y, bx;

    /* Scrim (dim screen) */
    alpha_blend_fill(dst, 0, 0, sw, sh, 0x000000, 120);

    /* Dialog body */
    draw_rounded_rect_alpha(dst, dx, dy, PWR_DLG_W, PWR_DLG_H, THEME_RADIUS_LG,
                            theme.mantle, 240);
    /* Border */
    alpha_blend_fill(dst, dx, dy, PWR_DLG_W, 1, theme.border, 180);
    alpha_blend_fill(dst, dx, dy + PWR_DLG_H - 1, PWR_DLG_W, 1, theme.border, 180);
    alpha_blend_fill(dst, dx, dy, 1, PWR_DLG_H, theme.border, 180);
    alpha_blend_fill(dst, dx + PWR_DLG_W - 1, dy, 1, PWR_DLG_H, theme.border, 180);

    /* Title */
    gui_surface_draw_string(dst, dx + (PWR_DLG_W - 18 * FONT_PSF_WIDTH) / 2,
                            dy + 20, "Que deseas hacer?", theme.text, 0, 0);

    /* Subtitle */
    gui_surface_draw_string(dst, dx + (PWR_DLG_W - 30 * FONT_PSF_WIDTH) / 2,
                            dy + 44, "Los cambios no guardados se", theme.dim, 0, 0);
    gui_surface_draw_string(dst, dx + (PWR_DLG_W - 10 * FONT_PSF_WIDTH) / 2,
                            dy + 62, "perderan.", theme.dim, 0, 0);

    /* Buttons row */
    brow_y = dy + PWR_DLG_H - 48;
    bx = dx + (PWR_DLG_W - 3 * PWR_BTN_W - 20) / 2;

    /* Cancel */
    draw_rounded_rect(dst, bx, brow_y, PWR_BTN_W, PWR_BTN_H, THEME_RADIUS_SM, theme.surface0);
    gui_surface_draw_string(dst, bx + (PWR_BTN_W - 7 * FONT_PSF_WIDTH) / 2,
                            brow_y + 6, "Cancelar", theme.text, 0, 0);
    bx += PWR_BTN_W + 10;

    /* Reboot */
    draw_rounded_rect(dst, bx, brow_y, PWR_BTN_W, PWR_BTN_H, THEME_RADIUS_SM, THEME_COL_WARNING);
    gui_surface_draw_string(dst, bx + (PWR_BTN_W - 8 * FONT_PSF_WIDTH) / 2,
                            brow_y + 6, "Reiniciar", theme_contrast_text(THEME_COL_WARNING), 0, 0);
    bx += PWR_BTN_W + 10;

    /* Shutdown */
    draw_rounded_rect(dst, bx, brow_y, PWR_BTN_W, PWR_BTN_H, THEME_RADIUS_SM, THEME_COL_ERROR);
    gui_surface_draw_string(dst, bx + (PWR_BTN_W - 6 * FONT_PSF_WIDTH) / 2,
                            brow_y + 6, "Apagar", theme_contrast_text(THEME_COL_ERROR), 0, 0);
}

static int power_dialog_handle_click(int mx, int my, int button) {
    int brow_y, bx;
    if (!power_dlg_open || button != 1) return 0;

    brow_y = power_dlg_y + PWR_DLG_H - 48;
    bx = power_dlg_x + (PWR_DLG_W - 3 * PWR_BTN_W - 20) / 2;

    if (my >= brow_y && my < brow_y + PWR_BTN_H) {
        /* Cancel */
        if (mx >= bx && mx < bx + PWR_BTN_W) {
            close_power_dialog();
            return 1;
        }
        bx += PWR_BTN_W + 10;
        /* Reboot */
        if (mx >= bx && mx < bx + PWR_BTN_W) {
            close_power_dialog();
            power_reboot();
            return 1;
        }
        bx += PWR_BTN_W + 10;
        /* Shutdown */
        if (mx >= bx && mx < bx + PWR_BTN_W) {
            close_power_dialog();
            power_shutdown();
            return 1;
        }
    }

    /* Click outside dialog = cancel */
    if (mx < power_dlg_x || mx >= power_dlg_x + PWR_DLG_W ||
        my < power_dlg_y || my >= power_dlg_y + PWR_DLG_H) {
        close_power_dialog();
        return 1;
    }

    return 1; /* consume click */
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
                            THEME_RADIUS_MD, COL_CTX_BG, 220);
    /* Top border accent */
    alpha_blend_fill(dst, dctx_menu_x + 3, dctx_menu_y, DCTX_W - 6, 1,
                     THEME_COL_ACCENT, 35);

    for (i = 0; i < dctx_item_count; i++) {
        int iy = dctx_menu_y + 4 + i * DCTX_ITEM_H;

        /* Hover highlight */
        if (i == dctx_hover)
            draw_rounded_rect_alpha(dst, dctx_menu_x + 4, iy,
                                    DCTX_W - 8, DCTX_ITEM_H - 2, THEME_RADIUS_SM,
                                    COL_CTX_HOVER, 60);

        gui_surface_draw_string(dst, dctx_menu_x + 14,
            iy + (DCTX_ITEM_H - FONT_PSF_HEIGHT) / 2,
            dctx_items[i].label,
            (i == dctx_hover) ? theme_contrast_text(COL_CTX_HOVER) : COL_CTX_TEXT, 0, 0);

        /* Separator after each item except last */
        if (i < dctx_item_count - 1)
            alpha_blend_fill(dst, dctx_menu_x + 8,
                iy + DCTX_ITEM_H - 2, DCTX_W - 16, 1,
                COL_CTX_BORDER, 80);
    }
}

static void draw_net_popup(gui_surface_t* dst) {
    int px = sw - NET_POPUP_W - 8;
    int py = sh - DOCK_H - NET_POPUP_H - 8;
    int ly;
    netif_t* iface = netif_get(0);

    draw_rounded_rect_alpha(dst, px, py, NET_POPUP_W, NET_POPUP_H, THEME_RADIUS_MD, COL_POPUP_BG, 220);
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
        draw_rounded_rect(dst, px + 10, ly, NET_POPUP_W - 20, 24, THEME_RADIUS_SM, COL_POPUP_BTN);
        gui_surface_draw_string(dst, px + 10 + (NET_POPUP_W - 20 - 18 * FONT_PSF_WIDTH) / 2,
            ly + (24 - FONT_PSF_HEIGHT) / 2,
            "Open Network Config", theme_contrast_text(COL_POPUP_BTN), 0, 0);
    }
}

static void open_start_menu(void) {
    start_menu_open = 1;
    menu_selected = 0;
    launcher_anim = 1; /* opening */
    launcher_anim_step = 0;
    launcher_cache_invalidate();
    close_context_menu();
    close_net_popup();
    {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - DOCK_H - LAUNCHER_H - 8;
        gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + DOCK_H + 8);
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

    /* ---- Populate dock (pinned icons at bottom of screen) ---- */
    dock_item_count = 0;
    dock_drag_idx = -1;
    dock_drag_active = 0;

    dock_items[dock_item_count].label = "Lyth OS";
    dock_items[dock_item_count].action = toggle_start_menu;
    dock_items[dock_item_count].icon_color = THEME_COL_ACCENT;
    dock_items[dock_item_count].shortcut = 'L';
    dock_items[dock_item_count].icon_pixels = icon_lyth_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_lyth_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_lyth_64_pixels;
    dock_item_count++;

    dock_items[dock_item_count].label = "Terminal";
    dock_items[dock_item_count].action = terminal_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_TERMINAL;
    dock_items[dock_item_count].shortcut = '>';
    dock_items[dock_item_count].icon_pixels = icon_terminal_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_terminal_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_terminal_64_pixels;
    dock_item_count++;

    dock_items[dock_item_count].label = "Calculator";
    dock_items[dock_item_count].action = calculator_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_CALC;
    dock_items[dock_item_count].shortcut = 'C';
    dock_items[dock_item_count].icon_pixels = icon_calc_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_calc_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_calc_64_pixels;
    dock_item_count++;

    dock_items[dock_item_count].label = "Files";
    dock_items[dock_item_count].action = filemanager_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_FILES;
    dock_items[dock_item_count].shortcut = 'F';
    dock_items[dock_item_count].icon_pixels = icon_nexus_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_nexus_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_nexus_64_pixels;
    dock_item_count++;

    dock_items[dock_item_count].label = "Settings";
    dock_items[dock_item_count].action = settings_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_SETTINGS;
    dock_items[dock_item_count].shortcut = 'S';
    dock_items[dock_item_count].icon_pixels = icon_settings_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_settings_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_settings_64_pixels;
    dock_item_count++;

    dock_items[dock_item_count].label = "Notes";
    dock_items[dock_item_count].action = editor_app_open;
    dock_items[dock_item_count].icon_color = COL_ICON_EDITOR;
    dock_items[dock_item_count].shortcut = 'N';
    dock_items[dock_item_count].icon_pixels = icon_notes_pixels;
    dock_items[dock_item_count].icon_pixels_24 = icon_notes_24_pixels;
    dock_items[dock_item_count].icon_pixels_64 = icon_notes_64_pixels;
    dock_item_count++;

    /* ---- Populate desktop shortcut icons ---- */
    dicon_count = 0;
    dicon_items[dicon_count].label    = "Terminal";
    dicon_items[dicon_count].action   = terminal_app_open;
    dicon_items[dicon_count].icon_pixels    = icon_terminal_pixels;
    dicon_items[dicon_count].icon_pixels_64 = icon_terminal_64_pixels;
    dicon_items[dicon_count].icon_color  = COL_ICON_TERMINAL;
    dicon_items[dicon_count].icon_letter = '>';
    dicon_count++;
    dicon_items[dicon_count].label    = "Files";
    dicon_items[dicon_count].action   = filemanager_app_open;
    dicon_items[dicon_count].icon_pixels    = icon_nexus_pixels;
    dicon_items[dicon_count].icon_pixels_64 = icon_nexus_64_pixels;
    dicon_items[dicon_count].icon_color  = COL_ICON_FILES;
    dicon_items[dicon_count].icon_letter = 'F';
    dicon_count++;
    dicon_items[dicon_count].label    = "Notes";
    dicon_items[dicon_count].action   = editor_app_open;
    dicon_items[dicon_count].icon_pixels    = icon_notes_pixels;
    dicon_items[dicon_count].icon_pixels_64 = icon_notes_64_pixels;
    dicon_items[dicon_count].icon_color  = COL_ICON_EDITOR;
    dicon_items[dicon_count].icon_letter = 'N';
    dicon_count++;
    dicon_items[dicon_count].label    = "Settings";
    dicon_items[dicon_count].action   = settings_app_open;
    dicon_items[dicon_count].icon_pixels    = icon_settings_pixels;
    dicon_items[dicon_count].icon_pixels_64 = icon_settings_64_pixels;
    dicon_items[dicon_count].icon_color  = COL_ICON_SETTINGS;
    dicon_items[dicon_count].icon_letter = 'S';
    dicon_count++;

    /* ---- Populate launcher (full app list shown in grid) ---- */
    launcher_item_count = 0;
    recent_count = 0;

#define ADD_LAUNCHER(lbl, fn, col, ch, ico32, ico24, ico64, pin, cat) do { \
    str_copy(launcher_items[launcher_item_count].label, (lbl), 24); \
    launcher_items[launcher_item_count].action = (fn); \
    launcher_items[launcher_item_count].icon_color = (col); \
    launcher_items[launcher_item_count].icon_letter = (ch); \
    launcher_items[launcher_item_count].icon_pixels = (ico32); \
    launcher_items[launcher_item_count].icon_pixels_24 = (ico24); \
    launcher_items[launcher_item_count].icon_pixels_64 = (ico64); \
    launcher_items[launcher_item_count].pinned = (pin); \
    launcher_items[launcher_item_count].category = (cat); \
    launcher_item_count++; \
} while(0)

    /* Core apps */
    ADD_LAUNCHER("Files",       filemanager_app_open, COL_ICON_FILES,    'F', icon_nexus_pixels,    icon_nexus_24_pixels,    icon_nexus_64_pixels,    1, CAT_CORE);
    ADD_LAUNCHER("Terminal",    terminal_app_open,    COL_ICON_TERMINAL, '>', icon_terminal_pixels,  icon_terminal_24_pixels, icon_terminal_64_pixels,  1, CAT_CORE);
    ADD_LAUNCHER("Notes",       editor_app_open,      COL_ICON_EDITOR,   'N', icon_notes_pixels,     icon_notes_24_pixels,    icon_notes_64_pixels,     1, CAT_CORE);
    ADD_LAUNCHER("Calculator",  calculator_app_open,  COL_ICON_CALC,     'C', icon_calc_pixels,      icon_calc_24_pixels,     icon_calc_64_pixels,      1, CAT_CORE);
    ADD_LAUNCHER("Viewer",      viewer_app_open,      COL_ICON_VIEWER,   'V', 0, 0, 0, 0, CAT_CORE);
    /* Utilities */
    ADD_LAUNCHER("Settings",    settings_app_open,    COL_ICON_SETTINGS, 'S', icon_settings_pixels,  icon_settings_24_pixels, icon_settings_64_pixels,  1, CAT_UTIL);
    ADD_LAUNCHER("Network",     netcfg_app_open,      COL_ICON_NETCFG,   'W', 0, 0, 0, 0, CAT_UTIL);
    /* System */
    ADD_LAUNCHER("Task Mgr",    taskman_app_open,     COL_ICON_TASKMAN,  'T', 0, 0, 0, 0, CAT_SYS);
    ADD_LAUNCHER("Sys Info",    sysinfo_app_open,     COL_ICON_SYSINFO,  'I', 0, 0, 0, 0, CAT_SYS);
    ADD_LAUNCHER("About",       about_app_open,       COL_ICON_ABOUT,    'A', icon_lyth_pixels, icon_lyth_24_pixels, icon_lyth_64_pixels, 0, CAT_SYS);

#undef ADD_LAUNCHER

    rebuild_desktop();
}

void desktop_shutdown(void) {
    gui_surface_free(&desk_surf);
}

void desktop_resize(int new_w, int new_h) {
    gui_surface_free(&desk_surf);
    sw = new_w;
    sh = new_h;
    tray_net_x = sw - 100;
    tray_menu_x = sw - 140;
    gui_surface_alloc(&desk_surf, sw, sh);
    desk_valid = 0;
    launcher_cache_valid = 0;
    rebuild_desktop();  /* pre-render wallpaper at new resolution */
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
        int npy = sh - DOCK_H - NET_POPUP_H - 8;
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
    /* ---- Dock hover scale animation (painted over baked dock) ---- */
    /* Skip entirely when launcher is open — it covers the dock area */
    if ((dock_hover_idx >= 0 || dock_scale > 0) && !start_menu_open) {
        int hover_i = dock_hover_idx >= 0 ? dock_hover_idx : -1;
        int dock_y = sh - DOCK_H;
        int dx = dock_start_x();
        int dy = dock_y + DOCK_Y_PAD;
        /* Restore baked dock area first (covers any previous hover + tooltip)
         * Clipped to the dirty region (x0,y0,x1,y1) to avoid writing outside
         * the compose rect and corrupting other overlays. */
        {
            int ax0 = dx - DOCK_SCALE_MAX;
            int ay0 = dock_y - DOCK_SCALE_MAX - FONT_PSF_HEIGHT - 16;
            int ax1 = dx + dock_total_w() + DOCK_SCALE_MAX;
            int ay1 = sh;
            int ar;
            if (ax0 < 0) ax0 = 0; if (ay0 < 0) ay0 = 0;
            if (ax1 > sw) ax1 = sw; if (ay1 > sh) ay1 = sh;
            /* Clip to dirty region */
            if (ax0 < x0) ax0 = x0; if (ay0 < y0) ay0 = y0;
            if (ax1 > x1) ax1 = x1; if (ay1 > y1) ay1 = y1;
            if (desk_surf.pixels && ax0 < ax1 && ay0 < ay1)
                for (ar = ay0; ar < ay1; ar++)
                    memcpy(&dst->pixels[ar * dst->stride + ax0],
                           &desk_surf.pixels[ar * sw + ax0],
                           (size_t)(ax1 - ax0) * 4);
        }
        /* Draw hovered icon scaled up + shifted up */
        if (hover_i >= 0 && dock_scale > 0) {
            int base_ix = dx + DOCK_ICON_PAD + hover_i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
            int base_iy = dy;
            int new_sz = DOCK_ICON_SIZE + dock_scale;
            int off = dock_scale / 2;
            int sx = base_ix - off;
            int sy = base_iy - dock_scale;  /* float upward */
            /* Subtle glow behind scaled icon */
            draw_rounded_rect_alpha(dst, sx - 2, sy - 2,
                                     new_sz + 4, new_sz + 4,
                                     THEME_RADIUS_MD, COL_DOCK_HOVER, 50);
            if (dock_items[hover_i].icon_pixels_64) {
                blit_icon_scaled(dst, sx, sy,
                                 dock_items[hover_i].icon_pixels_64, 64, 64,
                                 new_sz, new_sz);
            } else if (dock_items[hover_i].icon_pixels) {
                blit_icon_scaled(dst, sx, sy,
                                 dock_items[hover_i].icon_pixels, 32, 32,
                                 new_sz, new_sz);
            } else {
                draw_rounded_rect(dst, sx, sy, new_sz, new_sz,
                                  THEME_RADIUS_MD, dock_items[hover_i].icon_color);
                gui_surface_draw_char(dst,
                    sx + (new_sz - FONT_PSF_WIDTH) / 2,
                    sy + (new_sz - FONT_PSF_HEIGHT) / 2,
                    (unsigned char)dock_items[hover_i].shortcut,
                    COL_TASKBAR_TEXT, 0, 0);
            }
            /* Tooltip label above icon */
            if (dock_items[hover_i].label && dock_scale >= DOCK_SCALE_MAX) {
                const char *tip = dock_items[hover_i].label;
                int tlen = (int)strlen(tip);
                int tip_w = tlen * FONT_PSF_WIDTH + 12;
                int tip_h = FONT_PSF_HEIGHT + 8;
                int tip_x = base_ix + DOCK_ICON_SIZE / 2 - tip_w / 2;
                int tip_y = sy - tip_h - 4;
                if (tip_x < 2) tip_x = 2;
                if (tip_x + tip_w > sw - 2) tip_x = sw - 2 - tip_w;
                draw_rounded_rect_alpha(dst, tip_x, tip_y, tip_w, tip_h,
                                         THEME_RADIUS_MD, 0x0D1117, 220);
                gui_surface_draw_string(dst, tip_x + 6,
                    tip_y + 4, tip, COL_TASKBAR_TEXT, 0, 0);
            }
        }
    }

    /* ---- Launcher (with slide + crossfade animation) ---- */
    if (start_menu_open || launcher_anim != 0) {
        int lx = (sw - LAUNCHER_W) / 2;
        int ly = sh - DOCK_H - LAUNCHER_H - 8;
        /* Slide offset: 0 = fully visible, LAUNCHER_H = fully hidden below */
        int anim_frac = launcher_anim_step;
        if (anim_frac > LAUNCHER_ANIM_STEPS) anim_frac = LAUNCHER_ANIM_STEPS;
        if (anim_frac < 0) anim_frac = 0;
        int slide_off = LAUNCHER_H * (LAUNCHER_ANIM_STEPS - anim_frac) / LAUNCHER_ANIM_STEPS;
        int visible_h = LAUNCHER_H - slide_off;
        /* Alpha for crossfade: 0 = transparent, 255 = opaque */
        int launch_alpha = anim_frac * 255 / LAUNCHER_ANIM_STEPS;
        if (launch_alpha > 255) launch_alpha = 255;
        if (launch_alpha < 0) launch_alpha = 0;
        if (visible_h <= 0) goto skip_launcher;

        if (!(lx + LAUNCHER_W <= x0 || x1 <= lx ||
              ly + slide_off + visible_h <= y0 || y1 <= ly + slide_off)) {

            /* Ensure cache is populated */
            if (!launcher_cache_valid || !launcher_cache.pixels)
                launcher_cache_render();

            if (launcher_cache.pixels) {
                /* Blit with alpha blending for crossfade */
                int bx0 = x0 > lx ? x0 : lx;
                int by0 = y0 > (ly + slide_off) ? y0 : (ly + slide_off);
                int bx1 = x1 < (lx + LAUNCHER_W) ? x1 : (lx + LAUNCHER_W);
                int by1 = y1 < (ly + slide_off + visible_h) ? y1 : (ly + slide_off + visible_h);
                if (bx0 < bx1 && by0 < by1) {
                    int br;
                    if (launch_alpha >= 255) {
                        /* Fully opaque — fast path */
                        for (br = by0; br < by1; br++) {
                            int cache_y = br - ly;
                            int cache_x = bx0 - lx;
                            memcpy(&dst->pixels[br * dst->stride + bx0],
                                   &launcher_cache.pixels[cache_y * LAUNCHER_W + cache_x],
                                   (size_t)(bx1 - bx0) * 4);
                        }
                    } else {
                        /* Alpha-blended crossfade */
                        int a = launch_alpha;
                        int inv_a = 255 - a;
                        for (br = by0; br < by1; br++) {
                            int cache_y = br - ly;
                            int bx;
                            for (bx = bx0; bx < bx1; bx++) {
                                int cache_x = bx - lx;
                                uint32_t src = launcher_cache.pixels[cache_y * LAUNCHER_W + cache_x];
                                uint32_t bg  = dst->pixels[br * dst->stride + bx];
                                uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
                                uint32_t br2 = (bg >> 16) & 0xFF, bg2 = (bg >> 8) & 0xFF, bb = bg & 0xFF;
                                uint32_t r = (sr * a + br2 * inv_a) / 255;
                                uint32_t g = (sg * a + bg2 * inv_a) / 255;
                                uint32_t b = (sb * a + bb  * inv_a) / 255;
                                dst->pixels[br * dst->stride + bx] = (r << 16) | (g << 8) | b;
                            }
                        }
                    }
                }
            }
        }
    }
skip_launcher:
    (void)0;

    /* ---- Power dialog (modal) ---- */
    if (power_dlg_open) {
        draw_power_dialog(dst);
    }
}

int desktop_is_overlay_open(void) {
    return start_menu_open || launcher_anim != 0 || power_dlg_open;
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

/* ---- Taskbar auto-hide API ---- */
int desktop_taskbar_autohide_get(void) { return tb_autohide; }
void desktop_taskbar_autohide_set(int on) {
    tb_autohide = on ? 1 : 0;
    if (!tb_autohide) {
        /* Ensure taskbar is visible when disabling auto-hide */
        tb_visible = 1;
        tb_anim = 1;
    }
    desk_valid = 0;
    gui_dirty_add(0, 0, sw, TASKBAR_H + 2);
}

void desktop_on_tick(void) {
    unsigned int old_sec = clock_str[4]; /* check last minute digit */
    int net_now = net_is_connected();
    update_clock();
    if (clock_str[4] != (char)old_sec || net_now != net_last_up) {
        desk_valid = 0;
        launcher_cache_invalidate();
        rebuild_desktop();
        /* dirty top taskbar and bottom dock */
        gui_dirty_add(0, 0, sw, TASKBAR_H);
        gui_dirty_add(0, sh - DOCK_H, sw, DOCK_H);
    }

}

void desktop_anim_tick(void) {
    static unsigned int last_anim_ms = 0;
    unsigned int now;
    int need_dock_anim = 0;
    int target_scale = (dock_hover_idx >= 0) ? DOCK_SCALE_MAX : 0;

    if (launcher_anim == 0 && dock_scale == target_scale && tb_anim == 0) return;

    now = timer_get_uptime_ms();
    if (now - last_anim_ms < 16) return; /* ~60fps */
    last_anim_ms = now;

    /* --- Dock hover scale animation --- */
    if (dock_scale != target_scale) {
        int old_scale = dock_scale;
        if (dock_scale < target_scale) {
            dock_scale += (DOCK_SCALE_MAX + DOCK_SCALE_STEPS - 1) / DOCK_SCALE_STEPS;
            if (dock_scale > target_scale) dock_scale = target_scale;
        } else {
            dock_scale -= (DOCK_SCALE_MAX + DOCK_SCALE_STEPS - 1) / DOCK_SCALE_STEPS;
            if (dock_scale < 0) dock_scale = 0;
        }
        if (dock_scale != old_scale) {
            int dx = dock_start_x();
            int tip_extra = FONT_PSF_HEIGHT + 20;
            gui_dirty_add(dx - DOCK_SCALE_MAX, sh - DOCK_H - DOCK_SCALE_MAX - tip_extra,
                          dock_total_w() + DOCK_SCALE_MAX * 2,
                          DOCK_H + DOCK_SCALE_MAX + tip_extra);
        }
        (void)need_dock_anim;
    }

    /* --- Taskbar auto-hide animation --- */
    if (tb_anim != 0) {
        int old_step = tb_anim_step;
        if (tb_anim > 0) {
            tb_anim_step++;
            if (tb_anim_step >= TB_AUTOHIDE_STEPS) {
                tb_anim_step = TB_AUTOHIDE_STEPS;
                tb_anim = 0;
            }
        } else {
            tb_anim_step--;
            if (tb_anim_step <= 0) {
                tb_anim_step = 0;
                tb_anim = 0;
            }
        }
        if (tb_anim_step != old_step) {
            desk_valid = 0;
            gui_dirty_add(0, 0, sw, TASKBAR_H + 2);
        }
    }

    /* --- Launcher slide animation --- */
    if (launcher_anim != 0) {
    int lx = (sw - LAUNCHER_W) / 2;
    int ly = sh - DOCK_H - LAUNCHER_H - 8;
    int old_step = launcher_anim_step;
    if (launcher_anim > 0) {
        launcher_anim_step++;
        if (launcher_anim_step >= LAUNCHER_ANIM_STEPS) {
            launcher_anim = 0;
            launcher_anim_step = LAUNCHER_ANIM_STEPS;
        }
    } else {
        launcher_anim_step--;
        if (launcher_anim_step <= 0) {
            launcher_anim = 0;
            launcher_anim_step = 0;
            start_menu_open = 0;
        }
    }
    /* Only dirty the incremental strip that changed */
    {
        int old_off = LAUNCHER_H * (LAUNCHER_ANIM_STEPS - old_step) / LAUNCHER_ANIM_STEPS;
        int new_off = LAUNCHER_H * (LAUNCHER_ANIM_STEPS - launcher_anim_step) / LAUNCHER_ANIM_STEPS;
        int strip_y, strip_h;
        if (new_off < old_off) {
            strip_y = ly + new_off;
            strip_h = old_off - new_off;
        } else {
            strip_y = ly + old_off;
            strip_h = new_off - old_off;
        }
        if (strip_h > 0)
            gui_dirty_add(lx, strip_y, LAUNCHER_W, strip_h);
    }
    }
}

void desktop_update_hover(int mx, int my, int buttons) {
    int dock_y = sh - DOCK_H;
    int old_hover = dock_hover_idx;
    dock_hover_idx = -1;

    /* ---- Taskbar auto-hide proximity detection ---- */
    if (tb_autohide) {
        if (my < TB_HOTZONE && !tb_visible) {
            /* mouse near top edge → reveal */
            tb_visible = 1;
            if (tb_anim_step < TB_AUTOHIDE_STEPS)
                tb_anim = 1; /* start show animation */
        } else if (my >= TASKBAR_H + 8 && tb_visible && !start_menu_open) {
            /* mouse moved away from taskbar → hide */
            tb_visible = 0;
            if (tb_anim_step > 0)
                tb_anim = -1; /* start hide animation */
        }
    }

    /* Check dock icons at bottom */
    if (my >= dock_y) {
        int dx = dock_start_x();
        int dy = dock_y + DOCK_Y_PAD;
        int i;
        for (i = 0; i < dock_item_count; i++) {
            int ix = dx + DOCK_ICON_PAD + i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
            int iy = dy;
            if (mx >= ix && mx < ix + DOCK_ICON_SIZE &&
                my >= iy && my < iy + DOCK_ICON_SIZE) {
                dock_hover_idx = i;
                break;
            }
        }
    }

    /* ---- Dock drag-reorder ---- */
    if (dock_drag_active && !(buttons & 0x01)) {
        /* Button released → end drag */
        dock_drag_active = 0;
        desk_valid = 0;
        gui_dirty_add(0, sh - DOCK_H, sw, DOCK_H);
    }
    if (dock_drag_active && (buttons & 0x01)) {
        /* Check if mouse has moved into adjacent item slot */
        int dx = dock_start_x();
        int cell = DOCK_ICON_SIZE + DOCK_ICON_PAD;
        int rel = mx - dx - DOCK_ICON_PAD + cell / 2;
        int target = rel / cell;
        if (target < 1) target = 1;  /* don't swap with item 0 (start menu) */
        if (target >= dock_item_count) target = dock_item_count - 1;
        if (target != dock_drag_idx) {
            dock_item_t tmp = dock_items[dock_drag_idx];
            if (target > dock_drag_idx) {
                int j;
                for (j = dock_drag_idx; j < target; j++)
                    dock_items[j] = dock_items[j + 1];
            } else {
                int j;
                for (j = dock_drag_idx; j > target; j--)
                    dock_items[j] = dock_items[j - 1];
            }
            dock_items[target] = tmp;
            dock_drag_idx = target;
            dock_hover_idx = target;
            desk_valid = 0;
            gui_dirty_add(0, sh - DOCK_H, sw, DOCK_H);
        }
    }
    if (!dock_drag_active && dock_drag_idx >= 0 && (buttons & 0x01)) {
        /* Check drag threshold */
        int dist = mx - dock_drag_start_mx;
        if (dist < 0) dist = -dist;
        if (dist >= DOCK_DRAG_THRESHOLD) {
            dock_drag_active = 1;
        }
    }

    /* Also update desktop context menu hover */
    if (dctx_menu_open) {
        int old_dh = dctx_hover;
        int dth = dctx_item_count * DCTX_ITEM_H + 8;
        if (mx >= dctx_menu_x && mx < dctx_menu_x + DCTX_W &&
            my >= dctx_menu_y && my < dctx_menu_y + dth) {
            dctx_hover = (my - dctx_menu_y - 4) / DCTX_ITEM_H;
            if (dctx_hover >= dctx_item_count) dctx_hover = -1;
        } else {
            dctx_hover = -1;
        }
        if (dctx_hover != old_dh)
            gui_dirty_add(dctx_menu_x, dctx_menu_y, DCTX_W, dth);
    }

    if (dock_hover_idx != old_hover) {
        /* Reset scale for immediate feedback on hover change */
        if (dock_hover_idx < 0) dock_scale = 0;
        /* Dirty the dock area + scale overshoot + tooltip for hover redraw */
        int dx = dock_start_x();
        int tip_extra = FONT_PSF_HEIGHT + 20;
        gui_dirty_add(dx - DOCK_SCALE_MAX, dock_y - DOCK_SCALE_MAX - tip_extra,
                      dock_total_w() + DOCK_SCALE_MAX * 2,
                      DOCK_H + DOCK_SCALE_MAX + tip_extra);
    }
}

int desktop_handle_click(int mx, int my, int button) {
    int tb_off = 0;
    int taskbar_y;
    int dock_y = sh - DOCK_H; /* bottom dock */
    if (tb_autohide && tb_anim_step < TB_AUTOHIDE_STEPS)
        tb_off = TASKBAR_H * (TB_AUTOHIDE_STEPS - tb_anim_step) / TB_AUTOHIDE_STEPS;
    taskbar_y = -tb_off;

    /* Power dialog is modal — consumes all clicks */
    if (power_dlg_open)
        return power_dialog_handle_click(mx, my, button);

    /* handle net popup clicks */
    if (net_popup_open) {
        int px = sw - NET_POPUP_W - 8;
        int py = sh - DOCK_H - NET_POPUP_H - 8;
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
                        gui_window_close_animated(target);
                    } else if (idx == 1) {
                        target->anim_minimizing = 1;
                        gui_window_anim_start(target, 0, THEME_ANIM_NORMAL);
                        gui_dirty_add(target->x, target->y,
                                      target->width, target->height);
                        desk_valid = 0;
                        gui_dirty_add(0, 0, sw, TASKBAR_H);
                        gui_dirty_add(0, sh - DOCK_H, sw, DOCK_H);
                    } else if (idx == 2) {
                        gui_dirty_add(target->x, target->y,
                                      target->width, target->height);
                        gui_window_move(target, 0, TASKBAR_H);
                        target->width = sw;
                        target->height = sh - TASKBAR_H - DOCK_H;
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
        int ly = sh - DOCK_H - LAUNCHER_H - 8;

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
                        open_power_dialog();
                        return 1;
                    }
                    bx += 90;
                    if (mx >= bx && mx < bx + 90) {
                        close_start_menu();
                        open_power_dialog();
                        return 1;
                    }
                }
            }

            /* Check Pinned / Recent / All Apps grid */
            if (button == 1) {
                int grid_x = lx + 20;
                int grid_y = ly + 60;

                /* --- Pinned row click (only when not searching) --- */
                if (launcher_search_len == 0) {
                    int pin_y = ly + 72;
                    int pcol = 0, pi;
                    for (pi = 0; pi < launcher_item_count && pcol < LAUNCHER_COLS; pi++) {
                        if (!launcher_items[pi].pinned) continue;
                        int cx = grid_x + pcol * LAUNCHER_CELL_W;
                        int cy = pin_y;
                        if (mx >= cx && mx < cx + LAUNCHER_CELL_W - 4 &&
                            my >= cy && my < cy + LAUNCHER_CELL_H - 4) {
                            recent_track(pi);
                            close_start_menu();
                            if (launcher_items[pi].action)
                                launcher_items[pi].action();
                            return 1;
                        }
                        pcol++;
                    }
                    grid_y = pin_y + LAUNCHER_CELL_H + 4;

                    /* --- Recent row click --- */
                    if (recent_count > 0) {
                        int rec_y = grid_y + 12; /* after separator + label */
                        int ri;
                        for (ri = 0; ri < recent_count; ri++) {
                            int idx = recent_apps[ri];
                            if (idx < 0 || idx >= launcher_item_count) continue;
                            int cx = grid_x + ri * LAUNCHER_CELL_W;
                            int cy = rec_y;
                            if (mx >= cx && mx < cx + LAUNCHER_CELL_W - 4 &&
                                my >= cy && my < cy + LAUNCHER_CELL_H - 4) {
                                recent_track(idx);
                                close_start_menu();
                                if (launcher_items[idx].action)
                                    launcher_items[idx].action();
                                return 1;
                            }
                        }
                        grid_y = rec_y + LAUNCHER_CELL_H + 4;
                    }
                    /* skip "All Apps" separator + label */
                    grid_y += 12;
                }

                /* --- All Apps grid click (filtered) --- */
                {
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
                            recent_track(fi);
                            close_start_menu();
                            if (launcher_items[fi].action)
                                launcher_items[fi].action();
                            return 1;
                        }
                        fpos++;
                    }
                }
            }
            return 1; /* consumed: inside launcher */
        }
        /* click outside launcher: close it */
        close_start_menu();
    }

    /* ---- Top Taskbar clicks (running apps + tray) ---- */
    if (my >= taskbar_y && my < taskbar_y + TASKBAR_H) {
        /* Left section: running app icons (non-pinned only) */
        {
            int lx = 8;
            int item_w = TB_ICON_SZ + TB_ICON_PAD * 2;
            int i, count = gui_window_count();
            for (i = 0; i < count; i++) {
                gui_window_t* w = gui_window_get(i);
                if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
                if (!gui_window_on_current_ws(w)) continue;
                /* Skip pinned apps */
                {
                    int pinned = 0, d;
                    for (d = 0; d < dock_item_count; d++) {
                        if (dock_items[d].label &&
                            str_starts_with(w->title, dock_items[d].label)) {
                            pinned = 1; break;
                        }
                    }
                    if (pinned) continue;
                }
                if (lx + item_w > sw / 2) break;

                if (mx >= lx && mx < lx + item_w) {
                    if (button == 2) {
                        close_start_menu();
                        ctx_menu_open = 1;
                        ctx_target_win = w;
                        ctx_menu_x = mx;
                        ctx_menu_y = TASKBAR_H;
                        if (ctx_menu_x + CTX_W > sw) ctx_menu_x = sw - CTX_W;
                        gui_dirty_add(ctx_menu_x, ctx_menu_y, CTX_W,
                                      CTX_MAX_ITEMS * CTX_ITEM_H + 4);
                        return 1;
                    }
                    /* Toggle minimize/restore */
                    if (w->flags & GUI_WIN_MINIMIZED) {
                        w->flags &= ~GUI_WIN_MINIMIZED;
                        w->alpha = 0;
                        gui_window_anim_start(w, 255, THEME_ANIM_NORMAL);
                        gui_window_focus(w);
                        w->needs_redraw = 1;
                    } else {
                        gui_window_focus(w);
                    }
                    desk_valid = 0;
                    gui_dirty_add(0, 0, sw, TASKBAR_H);
                    gui_dirty_add(w->x, w->y,
                                  w->width + THEME_SHADOW_EXTENT,
                                  w->height + THEME_SHADOW_EXTENT);
                    return 1;
                }
                lx += item_w + 4;
            }
        }

        /* Center: workspace indicator clicks */
        {
            int ws_pill_w = 18, ws_gap = 4;
            int ws_total = GUI_MAX_WORKSPACES * ws_pill_w + (GUI_MAX_WORKSPACES - 1) * ws_gap;
            int ws_x = (sw - ws_total) / 2;
            int ws_y = taskbar_y + (TASKBAR_H - 10) / 2;
            if (my >= ws_y && my < ws_y + 10) {
                int wsi;
                for (wsi = 0; wsi < GUI_MAX_WORKSPACES; wsi++) {
                    if (mx >= ws_x && mx < ws_x + ws_pill_w) {
                        gui_workspace_switch(wsi);
                        desk_valid = 0;
                        return 1;
                    }
                    ws_x += ws_pill_w + ws_gap;
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
                    int py = sh - DOCK_H - NET_POPUP_H - 8;
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

        return 1; /* consumed: click was on top taskbar */
    }

    /* ---- Bottom Dock clicks (Lyth icon + pinned apps) ---- */
    if (my >= dock_y) {
        int dx = dock_start_x();
        int dy = dock_y + DOCK_Y_PAD;
        int i;

        /* End any existing dock drag */
        if (dock_drag_active) {
            dock_drag_active = 0;
            dock_drag_idx = -1;
            return 1;
        }

        for (i = 0; i < dock_item_count; i++) {
            int ix = dx + DOCK_ICON_PAD + i * (DOCK_ICON_SIZE + DOCK_ICON_PAD);
            int iy = dy;
            if (mx >= ix && mx < ix + DOCK_ICON_SIZE &&
                my >= iy && my < iy + DOCK_ICON_SIZE && button == 1) {
                if (i > 0) {
                    /* Start potential drag (item 0 = start menu, skip) */
                    dock_drag_idx = i;
                    dock_drag_start_mx = mx;
                }
                if (dock_items[i].action)
                    dock_items[i].action();
                return 1;
            }
        }
        dock_drag_idx = -1;
        return 1; /* consumed: click was on dock */
    }

    /* ---- Desktop shortcut icon click ---- */
    if (button == 1 && my > TASKBAR_H && my < dock_y && dicon_count > 0) {
        int di;
        int base_x = sw - DICON_PAD - DICON_CELL;
        int base_y = TASKBAR_H + DICON_PAD;
        int rows_max = (sh - TASKBAR_H - DOCK_H - 2 * DICON_PAD) / DICON_CELL;
        if (rows_max < 1) rows_max = 1;
        for (di = 0; di < dicon_count; di++) {
            int col = di / rows_max;
            int row = di % rows_max;
            int cx = base_x - col * DICON_CELL;
            int cy = base_y + row * DICON_CELL;
            if (mx >= cx && mx < cx + DICON_CELL &&
                my >= cy && my < cy + DICON_CELL) {
                if (dicon_items[di].action)
                    dicon_items[di].action();
                return 1;
            }
        }
    }

    /* ---- Desktop background: right-click opens desktop context menu ---- */
    if (button == 2 && my > TASKBAR_H && my < dock_y) {
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
            if (dctx_menu_y + dth > sh - DOCK_H) dctx_menu_y = sh - DOCK_H - dth;
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
        int ly = sh - DOCK_H - LAUNCHER_H - 8;

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
            launcher_cache_invalidate();
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_DOWN) {
            if (menu_selected + LAUNCHER_COLS < fcount)
                menu_selected += LAUNCHER_COLS;
            launcher_cache_invalidate();
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_LEFT) {
            if (menu_selected > 0) menu_selected--;
            launcher_cache_invalidate();
            gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            return 1;
        }
        if (event_type == INPUT_EVENT_RIGHT) {
            if (menu_selected < fcount - 1) menu_selected++;
            launcher_cache_invalidate();
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
                    recent_track(actual);
                    close_start_menu();
                    gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H + DOCK_H + 8);
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
                launcher_cache_invalidate();
                gui_dirty_add(lx, ly, LAUNCHER_W, LAUNCHER_H);
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_BACKSPACE) {
            if (launcher_search_len > 0) {
                launcher_search[--launcher_search_len] = '\0';
                menu_selected = 0;
                launcher_cache_invalidate();
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
    return TASKBAR_H;
}

gui_surface_t* desktop_get_surface(void) {
    return &desk_surf;
}

void desktop_invalidate_taskbar(void) {
    desk_valid = 0;
    gui_dirty_add(0, 0, sw, TASKBAR_H);
    gui_dirty_add(0, sh - DOCK_H, sw, DOCK_H);
}

void desktop_invalidate_all(void) {
    desk_valid = 0;
    gui_dirty_screen();
}
