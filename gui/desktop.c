/*
 * Desktop: background gradient, taskbar (bottom), start menu, and context menus.
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
#include "input.h"
#include "string.h"
#include "timer.h"
#include "rtc.h"
#include "physmem.h"
#include "netif.h"

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
#define COL_MENU_HOVER   0x3B82F6
#define COL_MENU_TEXT    0xCDD6F4
#define COL_MENU_SEP     0x45475A
#define COL_MENU_SEL_TEXT 0xFFFFFF

#define COL_WIN_ITEM_BG  0x2A2B3D
#define COL_WIN_ITEM_ACT 0x3B82F6

#define COL_CLOCK_TEXT   0xCDD6F4

#define COL_NET_GREEN    0x40C057
#define COL_NET_RED      0xF03E3E
#define COL_NET_ICON_BG  0x2A2B3D

#define COL_POPUP_BG     0x1E1E2E
#define COL_POPUP_BORDER 0x45475A
#define COL_POPUP_TEXT   0xCDD6F4
#define COL_POPUP_DIM    0x6C7086
#define COL_POPUP_BTN    0x3B82F6

#define COL_CTX_BG      0x2A2B3D
#define COL_CTX_HOVER   0x3B82F6
#define COL_CTX_TEXT     0xCDD6F4
#define COL_CTX_BORDER   0x45475A

/* ---- dimensions ---- */
#define TASKBAR_H       GUI_TASKBAR_HEIGHT
#define MENUBAR_H       0   /* no top menu bar - everything in taskbar */
#define START_BTN_W     56
#define START_BTN_H     (TASKBAR_H - 6)
#define CLOCK_W         70
#define NET_ICON_W      22
#define NET_ICON_GAP    6
#define NET_TRAY_W      (CLOCK_W + NET_ICON_W + NET_ICON_GAP)

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

/* ---- context menu ---- */
#define CTX_W           140
#define CTX_ITEM_H      24
#define CTX_MAX_ITEMS   3

static int ctx_menu_open;
static int ctx_menu_x, ctx_menu_y;
static gui_window_t* ctx_target_win;  /* window the context menu refers to */

/* ---- state ---- */
static int sw, sh;
static gui_surface_t desk_surf;    /* cached desktop background */
static int desk_valid;
static unsigned int desk_cache_sec;
static int start_menu_open;
static int menu_selected;     /* selected item index, -1 = none */

/* ---- clock ---- */
static char clock_str[9] = "00:00:00";

/* ---- network popup ---- */
#define NET_POPUP_W  220
#define NET_POPUP_H  160
static int net_popup_open;
static int net_last_up;   /* cached for dirty detection */

/* ==================================================================*/

static int net_is_connected(void) {
    netif_t* iface = netif_get(0);
    return (iface && iface->up && iface->ip_addr != 0);
}

/* tiny 9x9 Ethernet status icon drawn with putpixel */
static void draw_net_icon(gui_surface_t* dst, int ox, int oy, int connected) {
    uint32_t col = connected ? COL_NET_GREEN : COL_NET_RED;
    int x, y;
    /* draw a small monitor/screen shape (9x7) */
    /* top bar */
    for (x = 1; x < 8; x++) gui_surface_putpixel(dst, ox + x, oy, col);
    /* sides */
    for (y = 1; y < 5; y++) {
        gui_surface_putpixel(dst, ox, oy + y, col);
        gui_surface_putpixel(dst, ox + 8, oy + y, col);
    }
    /* bottom bar of screen */
    for (x = 1; x < 8; x++) gui_surface_putpixel(dst, ox + x, oy + 5, col);
    /* stand */
    gui_surface_putpixel(dst, ox + 4, oy + 6, col);
    /* base */
    for (x = 2; x < 7; x++) gui_surface_putpixel(dst, ox + x, oy + 7, col);
    /* fill inside if connected */
    if (connected) {
        for (y = 2; y < 4; y++)
            for (x = 3; x < 6; x++)
                gui_surface_putpixel(dst, ox + x, oy + y, col);
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
            if (bx + item_w > sw - NET_TRAY_W - 10) break;

            uint32_t bg = (w->flags & GUI_WIN_FOCUSED) ? COL_WIN_ITEM_ACT : COL_WIN_ITEM_BG;
            gui_surface_fill(&desk_surf, bx, taskbar_y + 5, item_w, TASKBAR_H - 10, bg);
            gui_surface_draw_string_n(&desk_surf, bx + 8,
                taskbar_y + (TASKBAR_H - FONT_PSF_HEIGHT) / 2,
                w->title, (item_w - 16) / FONT_PSF_WIDTH,
                COL_TASKBAR_TEXT, 0, 0);
            bx += item_w + 4;
        }
    }

    /* network status icon (left of clock) */
    {
        int icon_x = sw - CLOCK_W - NET_ICON_W - NET_ICON_GAP;
        int icon_y = taskbar_y + (TASKBAR_H - 9) / 2;  /* 9px icon height, centred */
        int connected = net_is_connected();
        net_last_up = connected;
        draw_net_icon(&desk_surf, icon_x + (NET_ICON_W - 9) / 2, icon_y, connected);
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
        /* highlight selected item */
        if (i == menu_selected) {
            gui_surface_fill(dst, mx + 2, iy, MENU_W - 4, MENU_ITEM_H, COL_MENU_HOVER);
            gui_surface_draw_string(dst, mx + 16, iy + (MENU_ITEM_H - FONT_PSF_HEIGHT) / 2,
                                    menu_items[i].label, COL_MENU_SEL_TEXT, 0, 0);
        } else {
            gui_surface_draw_string(dst, mx + 16, iy + (MENU_ITEM_H - FONT_PSF_HEIGHT) / 2,
                                    menu_items[i].label, COL_MENU_TEXT, 0, 0);
        }
        if (i < menu_item_count - 1) {
            gui_surface_hline(dst, mx + 8, iy + MENU_ITEM_H - 1, MENU_W - 16, COL_MENU_SEP);
        }
    }
}

static void draw_context_menu(gui_surface_t* dst) {
    static const char* ctx_labels[CTX_MAX_ITEMS] = { "Close", "Minimize", "Maximize" };
    int i;
    int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;

    gui_surface_fill(dst, ctx_menu_x, ctx_menu_y, CTX_W, total_h, COL_CTX_BG);
    /* border */
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
    int menu_h = menu_item_count * MENU_ITEM_H + 8;
    int menu_y = sh - TASKBAR_H - menu_h;
    start_menu_open = 0;
    menu_selected = -1;
    gui_dirty_add(3, menu_y, MENU_W, menu_h);
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
    int px = sw - NET_POPUP_W - 4;
    int py = sh - TASKBAR_H - NET_POPUP_H - 4;
    gui_dirty_add(px, py, NET_POPUP_W, NET_POPUP_H);
    net_popup_open = 0;
}

static void draw_net_popup(gui_surface_t* dst) {
    int px = sw - NET_POPUP_W - 4;
    int py = sh - TASKBAR_H - NET_POPUP_H - 4;
    int ly;
    netif_t* iface = netif_get(0);

    gui_surface_fill(dst, px, py, NET_POPUP_W, NET_POPUP_H, COL_POPUP_BG);
    /* border */
    gui_surface_hline(dst, px, py, NET_POPUP_W, COL_POPUP_BORDER);
    gui_surface_hline(dst, px, py + NET_POPUP_H - 1, NET_POPUP_W, COL_POPUP_BORDER);
    {
        int by;
        for (by = py; by < py + NET_POPUP_H; by++) {
            gui_surface_putpixel(dst, px, by, COL_POPUP_BORDER);
            gui_surface_putpixel(dst, px + NET_POPUP_W - 1, by, COL_POPUP_BORDER);
        }
    }

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
        gui_surface_fill(dst, px + 10, ly, NET_POPUP_W - 20, 24, COL_POPUP_BTN);
        gui_surface_draw_string(dst, px + 10 + (NET_POPUP_W - 20 - 18 * FONT_PSF_WIDTH) / 2,
            ly + (24 - FONT_PSF_HEIGHT) / 2,
            "Open Network Config", COL_START_TEXT, 0, 0);
    }
}

static void open_start_menu(void) {
    start_menu_open = 1;
    menu_selected = 0;
    close_context_menu();
    close_net_popup();
    {
        int menu_h = menu_item_count * MENU_ITEM_H + 8;
        gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h + TASKBAR_H);
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

    /* overlay context menu if open */
    if (ctx_menu_open) {
        draw_context_menu(dst);
    }

    /* overlay network popup if open */
    if (net_popup_open) {
        draw_net_popup(dst);
    }
}

void desktop_on_tick(void) {
    unsigned int old_sec = clock_str[7];
    int net_now = net_is_connected();
    update_clock();
    if (clock_str[7] != (char)old_sec || net_now != net_last_up) {
        desk_valid = 0;  /* force taskbar redraw */
        rebuild_desktop();
        /* dirty only the taskbar area */
        gui_dirty_add(0, sh - TASKBAR_H, sw, TASKBAR_H);
    }
}

int desktop_handle_click(int mx, int my, int button) {
    int taskbar_y = sh - TASKBAR_H;

    /* handle net popup clicks */
    if (net_popup_open) {
        int px = sw - NET_POPUP_W - 4;
        int py = sh - TASKBAR_H - NET_POPUP_H - 4;
        if (mx >= px && mx < px + NET_POPUP_W &&
            my >= py && my < py + NET_POPUP_H) {
            /* check if clicking "Open Network Config" button */
            netif_t* iface = netif_get(0);
            if (iface && button == 1) {
                int connected = (iface->up && iface->ip_addr != 0);
                int ly = py + 8 + FONT_PSF_HEIGHT + 10;
                ly += FONT_PSF_HEIGHT + 6;  /* status */
                ly += FONT_PSF_HEIGHT + 4;  /* interface */
                if (connected) ly += FONT_PSF_HEIGHT + 4;  /* IP */
                ly += 6;
                if (my >= ly && my < ly + 24 && mx >= px + 10 && mx < px + NET_POPUP_W - 10) {
                    close_net_popup();
                    netcfg_app_open();
                    return 1;
                }
            }
            return 1; /* consumed: inside popup */
        }
        close_net_popup();
        /* fall through */
    }

    /* close context menu if clicking outside it */
    if (ctx_menu_open) {
        int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
        if (mx >= ctx_menu_x && mx < ctx_menu_x + CTX_W &&
            my >= ctx_menu_y && my < ctx_menu_y + total_h) {
            /* click inside context menu */
            if (button == 1 && ctx_target_win) {
                int idx = (my - ctx_menu_y - 2) / CTX_ITEM_H;
                gui_window_t* target = ctx_target_win;
                close_context_menu();
                if (idx >= 0 && idx < CTX_MAX_ITEMS && target) {
                    if (idx == 0) {
                        /* Close */
                        gui_dirty_add(target->x - 6, target->y - 6,
                                      target->width + 12, target->height + 12);
                        if (target->on_close) target->on_close(target);
                        else gui_window_destroy(target);
                    } else if (idx == 1) {
                        /* Minimize */
                        target->flags |= GUI_WIN_MINIMIZED;
                        gui_dirty_add(target->x, target->y,
                                      target->width, target->height);
                        desk_valid = 0;
                        gui_dirty_add(0, taskbar_y, sw, TASKBAR_H);
                    } else if (idx == 2) {
                        /* Maximize */
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

    /* start menu click handling */
    if (start_menu_open) {
        int menu_x = 3;
        int menu_h = menu_item_count * MENU_ITEM_H + 8;
        int menu_y = sh - TASKBAR_H - menu_h;

        if (mx >= menu_x && mx < menu_x + MENU_W &&
            my >= menu_y && my < menu_y + menu_h) {
            /* which item? */
            int idx = (my - menu_y - 4) / MENU_ITEM_H;
            if (idx >= 0 && idx < menu_item_count && button == 1) {
                close_start_menu();
                gui_dirty_add(menu_x, menu_y, MENU_W, menu_h + TASKBAR_H);
                if (menu_items[idx].action)
                    menu_items[idx].action();
                return 1;
            }
        }
        /* click outside menu: close it */
        close_start_menu();
        /* fall through to check taskbar clicks */
    }

    /* taskbar clicks */
    if (my >= taskbar_y) {
        /* start button */
        if (mx >= 3 && mx < 3 + START_BTN_W && button == 1) {
            toggle_start_menu();
            return 1;
        }

        /* window items in taskbar */
        {
            int i, count = gui_window_count();
            int bx = START_BTN_W + 12;
            for (i = 0; i < count; i++) {
                gui_window_t* w = gui_window_get(i);
                if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
                int item_w = (int)strlen(w->title) * FONT_PSF_WIDTH + 16;
                if (item_w > 140) item_w = 140;

                if (mx >= bx && mx < bx + item_w) {
                    if (button == 2) {
                        /* right-click: open context menu */
                        close_start_menu();
                        ctx_menu_open = 1;
                        ctx_target_win = w;
                        ctx_menu_x = mx;
                        /* position above taskbar */
                        {
                            int total_h = CTX_MAX_ITEMS * CTX_ITEM_H + 4;
                            ctx_menu_y = taskbar_y - total_h;
                        }
                        if (ctx_menu_x + CTX_W > sw) ctx_menu_x = sw - CTX_W;
                        gui_dirty_add(ctx_menu_x, ctx_menu_y, CTX_W,
                                      CTX_MAX_ITEMS * CTX_ITEM_H + 4);
                        return 1;
                    }
                    /* left-click: focus / unminimize */
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

        /* network icon click */
        {
            int icon_x = sw - CLOCK_W - NET_ICON_W - NET_ICON_GAP;
            if (mx >= icon_x && mx < icon_x + NET_ICON_W && button == 1) {
                close_start_menu();
                close_context_menu();
                if (net_popup_open) {
                    close_net_popup();
                } else {
                    net_popup_open = 1;
                    int px = sw - NET_POPUP_W - 4;
                    int py = sh - TASKBAR_H - NET_POPUP_H - 4;
                    gui_dirty_add(px, py, NET_POPUP_W, NET_POPUP_H);
                }
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

    /* start menu keyboard navigation */
    if (start_menu_open) {
        if (event_type == INPUT_EVENT_UP) {
            if (menu_selected > 0) menu_selected--;
            else menu_selected = menu_item_count - 1;
            {
                int menu_h = menu_item_count * MENU_ITEM_H + 8;
                gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h);
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_DOWN) {
            if (menu_selected < menu_item_count - 1) menu_selected++;
            else menu_selected = 0;
            {
                int menu_h = menu_item_count * MENU_ITEM_H + 8;
                gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h);
            }
            return 1;
        }
        if (event_type == INPUT_EVENT_ENTER) {
            if (menu_selected >= 0 && menu_selected < menu_item_count) {
                void (*action)(void) = menu_items[menu_selected].action;
                close_start_menu();
                {
                    int menu_h = menu_item_count * MENU_ITEM_H + 8;
                    gui_dirty_add(3, sh - TASKBAR_H - menu_h, MENU_W, menu_h + TASKBAR_H);
                }
                if (action) action();
            }
            return 1;
        }
        /* Escape closes the menu */
        if (event_type == INPUT_EVENT_CHAR && key == 27) {
            close_start_menu();
            return 1;
        }
    }

    /* Escape closes net popup too */
    if (net_popup_open && event_type == INPUT_EVENT_CHAR && key == 27) {
        close_net_popup();
        return 1;
    }

    return 0;
}

int desktop_get_menubar_height(void) {
    return MENUBAR_H;
}

gui_surface_t* desktop_get_surface(void) {
    return &desk_surf;
}
