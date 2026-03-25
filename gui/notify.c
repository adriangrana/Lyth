/* ============================================================
 *  notify.c  —  Notification Center (toast popups)
 *
 *  Notifications appear as toasts in the top-right corner
 *  and auto-dismiss after a timeout.
 * ============================================================ */

#include "notify.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"

/* ---- Colours ---- */
#define COL_N_BG     0x313244
#define COL_N_BORDER 0x45475A
#define COL_N_TITLE  0xCDD6F4
#define COL_N_BODY   0xA6ADC8
#define COL_N_DIM    0x6C7086

/* ---- Layout ---- */
#define NOTIFY_W     260
#define NOTIFY_H     56
#define NOTIFY_PAD   8
#define NOTIFY_GAP   4
#define NOTIFY_RIGHT_MARGIN 10
#define NOTIFY_TOP_MARGIN   10
#define NOTIFY_DURATION_MS  4000

/* ---- Queue ---- */
#define MAX_TOASTS   4
#define NOTIFY_TITLE_MAX 32
#define NOTIFY_BODY_MAX  64

typedef struct {
    char title[NOTIFY_TITLE_MAX];
    char body[NOTIFY_BODY_MAX];
    unsigned int expire_ms;
    int active;
} toast_t;

static toast_t toasts[MAX_TOASTS];

/* ---- Public API ---- */

void notify_push(const char* title, const char* body) {
    int i;
    int slot = -1;

    /* Find empty slot, or recycle oldest */
    for (i = 0; i < MAX_TOASTS; i++) {
        if (!toasts[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        /* Shift all down, discard oldest */
        for (i = 0; i < MAX_TOASTS - 1; i++)
            toasts[i] = toasts[i + 1];
        slot = MAX_TOASTS - 1;
    }

    str_copy(toasts[slot].title, title ? title : "", NOTIFY_TITLE_MAX);
    str_copy(toasts[slot].body, body ? body : "", NOTIFY_BODY_MAX);
    toasts[slot].expire_ms = timer_get_uptime_ms() + NOTIFY_DURATION_MS;
    toasts[slot].active = 1;

    gui_request_redraw(); /* trigger redraw */
}

void notify_tick(void) {
    int i;
    unsigned int now = timer_get_uptime_ms();
    for (i = 0; i < MAX_TOASTS; i++) {
        if (toasts[i].active && now >= toasts[i].expire_ms) {
            toasts[i].active = 0;
            gui_request_redraw(); /* trigger repaint */
        }
    }
}

int notify_count(void) {
    int i, c = 0;
    for (i = 0; i < MAX_TOASTS; i++)
        if (toasts[i].active) c++;
    return c;
}

void notify_paint(void* surface, int screen_w) {
    gui_surface_t* s = (gui_surface_t*)surface;
    int i, y;
    int drawn = 0;

    if (!s || !s->pixels) return;

    y = NOTIFY_TOP_MARGIN;

    for (i = 0; i < MAX_TOASTS; i++) {
        if (!toasts[i].active) continue;

        int nx = screen_w - NOTIFY_W - NOTIFY_RIGHT_MARGIN;
        int ny = y;

        /* Background */
        gui_surface_fill(s, nx, ny, NOTIFY_W, NOTIFY_H, COL_N_BG);

        /* Border (top and bottom) */
        gui_surface_hline(s, nx, ny, NOTIFY_W, COL_N_BORDER);
        gui_surface_hline(s, nx, ny + NOTIFY_H - 1, NOTIFY_W, COL_N_BORDER);
        /* Sides */
        {
            int r;
            for (r = ny; r < ny + NOTIFY_H; r++) {
                gui_surface_putpixel(s, nx, r, COL_N_BORDER);
                gui_surface_putpixel(s, nx + NOTIFY_W - 1, r, COL_N_BORDER);
            }
        }

        /* Title */
        gui_surface_draw_string_n(s, nx + NOTIFY_PAD, ny + 6,
                                  toasts[i].title,
                                  (NOTIFY_W - 2 * NOTIFY_PAD) / GUI_FONT_W,
                                  COL_N_TITLE, 0, 0);

        /* Body */
        gui_surface_draw_string_n(s, nx + NOTIFY_PAD, ny + 6 + GUI_FONT_H + 4,
                                  toasts[i].body,
                                  (NOTIFY_W - 2 * NOTIFY_PAD) / GUI_FONT_W,
                                  COL_N_BODY, 0, 0);

        y += NOTIFY_H + NOTIFY_GAP;
        drawn++;
    }
}
