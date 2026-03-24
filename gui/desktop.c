/*
 * Desktop: background gradient, taskbar (bottom), and start menu.
 *
 * The desktop caches its background (gradient + taskbar chrome) in a surface
 * that is only rebuilt when the screen size changes or the clock ticks.
 * The compositor blits from this cached surface into dirty regions, which
 * is very fast (just memcpy of scanlines).
 */

#include "desktop.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"
#include "physmem.h"

/* forward declarations for app launchers */
void terminal_app_open(void);
void taskman_app_open(void);
void sysinfo_app_open(void);
void netcfg_app_open(void);
void settings_app_open(void);

/* ---- colours ---- */
#define COL_BG_TOP       0x1A1B2E
#define COL_BG_MID       0x16213E
#define COL_BG_BOT       0x0F3460

#define COL_TASKBAR_BG   0x1E1E2E
#define COL_TASKBAR_SEP  0x313244
#define COL_TASKBAR_TEXT 0xCDD6F4
#define COL_TASKBAR_DIM  0x6C7086

#define COL_START_BG     0x3B82F6
#define COL_START_TEXT   0xFFFFFF

#define COL_MENU_BG      0x1E1E2E
#define COL_MENU_HOVER   0x313244
#define COL_MENU_TEXT    0xCDD6F4
#define COL_MENU_SEP     0x45475A

#define COL_WIN_ITEM_BG  0x2A2B3D
#define COL_WIN_ITEM_ACT 0x3B82F6

#define COL_CLOCK_TEXT   0xCDD6F4

/* ---- dimensions ---- */
#define TASKBAR_H       GUI_TASKBAR_HEIGHT
#define MENUBAR_H       0   /* no top menu bar - everything in taskbar */
#define START_BTN_W     56
#define START_BTN_H     (TASKBAR_H - 6)
#define CLOCK_W         70

/* ---- start menu ---- */
#define MENU_W          200
#define MENU_ITEM_H     28
#define MENU_MAX_ITEMS  8

typedef struct {
    char label[32];
    void (*action)(void);
} menu_item_t;

static menu_item_t menu_items[MENU_MAX_ITEMS];
static int menu_item_count;

/* ---- state ---- */
static int sw, sh;
static gui_surface_t desk_surf;    /* cached desktop background */
static int desk_valid;
static unsigned int desk_cache_sec;
static int start_menu_open;

/* ---- clock ---- */
static char clock_str[9] = "00:00:00";

/* ==================================================================*/

static uint32_t mix_rgb(uint32_t a, uint32_t b, int t, int max) {
    if (max <= 0) max = 1;
    uint32_t ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    uint32_t rr = (ar * (uint32_t)(max - t) + br * (uint32_t)t) / (uint32_t)max;
    uint32_t rg = (ag * (uint32_t)(max - t) + bg * (uint32_t)t) / (uint32_t)max;
    uint32_t rb = (ab * (uint32_t)(max - t) + bb * (uint32_t)t) / (uint32_t)max;
    return (rr << 16) | (rg << 8) | rb;
}

static void update_clock(void) {
    rtc_time_t rtc;
    rtc_read(&rtc);
    clock_str[0] = '0' + (rtc.hour / 10) % 10;
    clock_str[1] = '0' + rtc.hour % 10;
    clock_str[2] = ':';
    clock_str[3] = '0' + (rtc.min / 10) % 10;
    clock_str[4] = '0' + rtc.min % 10;
    clock_str[5] = ':';
    clock_str[6] = '0' + (rtc.sec / 10) % 10;
    clock_str[7] = '0' + rtc.sec % 10;
    clock_str[8] = '\0';
}

static void rebuild_desktop(void) {
    int y, taskbar_y;
    int half = sh / 2;

    if (!desk_surf.pixels) return;

    /* gradient background */
    for (y = 0; y < half; y++) {
        uint32_t c = mix_rgb(COL_BG_TOP, COL_BG_MID, y, half > 1 ? half - 1 : 1);
        memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
    }
    for (y = half; y < sh - TASKBAR_H; y++) {
        uint32_t c = mix_rgb(COL_BG_MID, COL_BG_BOT, y - half,
                             (sh - TASKBAR_H - half) > 1 ? (sh - TASKBAR_H - half - 1) : 1);
        memset32(&desk_surf.pixels[y * sw], c, (size_t)sw);
    }

    /* taskbar */
    taskbar_y = sh - TASKBAR_H;
    gui_surface_fill(&desk_surf, 0, taskbar_y, sw, TASKBAR_H, COL_TASKBAR_BG);
    gui_surface_hline(&desk_surf, 0, taskbar_y, sw, COL_TASKBAR_SEP);

    /* start button */
    gui_surface_fill(&desk_surf, 3, taskbar_y + 3, START_BTN_W, START_BTN_H, COL_START_BG);
    gui_surface_draw_string(&desk_surf,
        3 + (START_BTN_W - 4 * FONT_PSF_WIDTH) / 2,
        taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
        "Lyth", COL_START_TEXT, 0, 0);

    /* window list in taskbar */
    {
        int i, count = gui_window_count();
        int bx = START_BTN_W + 12;
        for (i = 0; i < count; i++) {
            gui_window_t* w = gui_window_get(i);
            if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
            int item_w = (int)strlen(w->title) * FONT_PSF_WIDTH + 16;
            if (item_w > 140) item_w = 140;
            if (bx + item_w > sw - CLOCK_W - 10) break;

            uint32_t bg = (w->flags & GUI_WIN_FOCUSED) ? COL_WIN_ITEM_ACT : COL_WIN_ITEM_BG;
            gui_surface_fill(&desk_surf, bx, taskbar_y + 5, item_w, TASKBAR_H - 10, bg);
            gui_surface_draw_string_n(&desk_surf, bx + 8,
                taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
                w->title, (item_w - 16) / FONT_PSF_WIDTH,
                COL_TASKBAR_TEXT, 0, 0);
            bx += item_w + 4;
        }
    }

    /* clock (right side of taskbar) */
    gui_surface_draw_string(&desk_surf, sw - CLOCK_W,
        taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
        clock_str, COL_CLOCK_TEXT, 0, 0);

    desk_valid = 1;
}

static void draw_start_menu(gui_surface_t* dst) {
    int mx = 3;
    int my = sh - TASKBAR_H - menu_item_count * MENU_ITEM_H - 8;
    int i;
    int menu_h = menu_item_count * MENU_ITEM_H + 8;

    gui_surface_fill(dst, mx, my, MENU_W, menu_h, COL_MENU_BG);
    /* top border */
    gui_surface_hline(dst, mx, my, MENU_W, COL_MENU_SEP);

    for (i = 0; i < menu_item_count; i++) {
        int iy = my + 4 + i * MENU_ITEM_H;
        gui_surface_draw_string(dst, mx + 16, iy + (MENU_ITEM_H - FONT_PSF_HEIGHT) / 2,
                                menu_items[i].label, COL_MENU_TEXT, 0, 0);
        if (i < menu_item_count - 1) {
            gui_surface_hline(dst, mx + 8, iy + MENU_ITEM_H - 1, MENU_W - 16, COL_MENU_SEP);
        }
    }
}

/* ==================================================================*/

void desktop_init(int screen_w, int screen_h) {
    sw = screen_w;
    sh = screen_h;
    start_menu_open = 0;
    desk_valid = 0;
    desk_cache_sec = 0xFFFFFFFF;

    gui_surface_alloc(&desk_surf, sw, sh);
    update_clock();

    /* populate start menu */
    menu_item_count = 0;

    memcpy(menu_items[menu_item_count].label, "Terminal", 9);
    menu_items[menu_item_count].action = terminal_app_open;
    menu_item_count++;

    memcpy(menu_items[menu_item_count].label, "Task Manager", 13);
    menu_items[menu_item_count].action = taskman_app_open;
    menu_item_count++;

    memcpy(menu_items[menu_item_count].label, "System Info", 12);
    menu_items[menu_item_count].action = sysinfo_app_open;
    menu_item_count++;

    memcpy(menu_items[menu_item_count].label, "Network", 8);
    menu_items[menu_item_count].action = netcfg_app_open;
    menu_item_count++;

    memcpy(menu_items[menu_item_count].label, "Settings", 9);
    menu_items[menu_item_count].action = settings_app_open;
    menu_item_count++;

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

    /* overlay start menu if open */
    if (start_menu_open) {
        draw_start_menu(dst);
    }
}

void desktop_on_tick(void) {
    unsigned int old_sec = clock_str[7];
    update_clock();
    if (clock_str[7] != (char)old_sec) {
        desk_valid = 0;  /* force taskbar redraw */
        rebuild_desktop();
        /* dirty only the taskbar area */
        gui_dirty_add(0, sh - TASKBAR_H, sw, TASKBAR_H);
    }
}

int desktop_handle_click(int mx, int my) {
    int taskbar_y = sh - TASKBAR_H;

    /* start menu click handling */
    if (start_menu_open) {
        int menu_x = 3;
        int menu_h = menu_item_count * MENU_ITEM_H + 8;
        int menu_y = sh - TASKBAR_H - menu_h;

        if (mx >= menu_x && mx < menu_x + MENU_W &&
            my >= menu_y && my < menu_y + menu_h) {
            /* which item? */
            int idx = (my - menu_y - 4) / MENU_ITEM_H;
            if (idx >= 0 && idx < menu_item_count) {
                start_menu_open = 0;
                gui_dirty_add(menu_x, menu_y, MENU_W, menu_h + TASKBAR_H);
                if (menu_items[idx].action)
                    menu_items[idx].action();
                return 1;
            }
        }
        /* click outside menu: close it */
        start_menu_open = 0;
        {
            int menu_yy = sh - TASKBAR_H - menu_h;
            gui_dirty_add(3, menu_yy, MENU_W, menu_h);
        }
        /* fall through to check taskbar clicks */
    }

    /* taskbar clicks */
    if (my >= taskbar_y) {
        /* start button */
        if (mx >= 3 && mx < 3 + START_BTN_W) {
            start_menu_open = !start_menu_open;
            if (start_menu_open) {
                int menu_h = menu_item_count * MENU_ITEM_H + 8;
                gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h + TASKBAR_H);
            } else {
                int menu_h = menu_item_count * MENU_ITEM_H + 8;
                gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h);
            }
            return 1;
        }

        /* window items in taskbar - click to focus/unminimize */
        {
            int i, count = gui_window_count();
            int bx = START_BTN_W + 12;
            for (i = 0; i < count; i++) {
                gui_window_t* w = gui_window_get(i);
                if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
                int item_w = (int)strlen(w->title) * FONT_PSF_WIDTH + 16;
                if (item_w > 140) item_w = 140;

                if (mx >= bx && mx < bx + item_w) {
                    if (w->flags & GUI_WIN_MINIMIZED) {
                        w->flags &= ~GUI_WIN_MINIMIZED;
                    }
                    gui_window_focus(w);
                    desk_valid = 0;
                    gui_dirty_add(0, taskbar_y, sw, TASKBAR_H);
                    gui_dirty_add(w->x, w->y, w->width, w->height);
                    return 1;
                }
                bx += item_w + 4;
            }
        }

        return 1; /* consumed: click was on taskbar */
    }

    return 0;
}

int desktop_handle_key(int event_type, char key) {
    (void)event_type;
    (void)key;
    /* future: keyboard shortcuts for start menu etc */
    return 0;
}

int desktop_get_menubar_height(void) {
    return MENUBAR_H;
}

gui_surface_t* desktop_get_surface(void) {
    return &desk_surf;
}
