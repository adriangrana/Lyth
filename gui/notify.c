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
#include "theme.h"

/* ---- Colours (from theme.h) ---- */
#define COL_N_BG     THEME_COL_SURFACE0
#define COL_N_BORDER THEME_COL_SURFACE1
#define COL_N_TITLE  THEME_COL_TEXT
#define COL_N_BODY   THEME_COL_SUBTEXT0
#define COL_N_DIM    THEME_COL_DIM

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

/* ---- Local alpha-blend helpers (matching desktop.c pattern) ---- */
static void n_alpha_fill(gui_surface_t* s, int x0, int y0, int w, int h,
                          uint32_t col, int alpha) {
    int row, cx;
    uint32_t fr = (col >> 16) & 0xFF;
    uint32_t fg = (col >> 8) & 0xFF;
    uint32_t fb = col & 0xFF;
    int ia = 255 - alpha;
    if (x0 < 0) { w += x0; x0 = 0; }
    if (y0 < 0) { h += y0; y0 = 0; }
    if (x0 + w > s->width) w = s->width - x0;
    if (y0 + h > s->height) h = s->height - y0;
    if (w <= 0 || h <= 0) return;
    for (row = y0; row < y0 + h; row++) {
        uint32_t *p = &s->pixels[row * s->stride + x0];
        for (cx = 0; cx < w; cx++) {
            uint32_t bg = p[cx];
            uint32_t r = (fr * (uint32_t)alpha + ((bg >> 16) & 0xFF) * (uint32_t)ia) / 255;
            uint32_t g = (fg * (uint32_t)alpha + ((bg >> 8) & 0xFF) * (uint32_t)ia) / 255;
            uint32_t b = (fb * (uint32_t)alpha + (bg & 0xFF) * (uint32_t)ia) / 255;
            p[cx] = (r << 16) | (g << 8) | b;
        }
    }
}

static void n_rounded_rect(gui_surface_t* s, int x, int y, int w, int h,
                            int r, uint32_t col, int alpha) {
    if (r < 1 || h < 4 || w < 4) { n_alpha_fill(s, x, y, w, h, col, alpha); return; }
    if (r > 3) r = 3;
    if (r == 1) {
        n_alpha_fill(s, x + 1, y, w - 2, 1, col, alpha);
        n_alpha_fill(s, x, y + 1, w, h - 2, col, alpha);
        n_alpha_fill(s, x + 1, y + h - 1, w - 2, 1, col, alpha);
    } else if (r == 2) {
        n_alpha_fill(s, x + 2, y, w - 4, 1, col, alpha);
        n_alpha_fill(s, x + 1, y + 1, w - 2, 1, col, alpha);
        n_alpha_fill(s, x, y + 2, w, h - 4, col, alpha);
        n_alpha_fill(s, x + 1, y + h - 2, w - 2, 1, col, alpha);
        n_alpha_fill(s, x + 2, y + h - 1, w - 4, 1, col, alpha);
    } else {
        n_alpha_fill(s, x + 3, y, w - 6, 1, col, alpha);
        n_alpha_fill(s, x + 2, y + 1, w - 4, 1, col, alpha);
        n_alpha_fill(s, x + 1, y + 2, w - 2, 1, col, alpha);
        n_alpha_fill(s, x, y + 3, w, h - 6, col, alpha);
        n_alpha_fill(s, x + 1, y + h - 3, w - 2, 1, col, alpha);
        n_alpha_fill(s, x + 2, y + h - 2, w - 4, 1, col, alpha);
        n_alpha_fill(s, x + 3, y + h - 1, w - 6, 1, col, alpha);
    }
}

void notify_paint(void* surface, int screen_w) {
    gui_surface_t* s = (gui_surface_t*)surface;
    int i, y;

    if (!s || !s->pixels) return;

    y = NOTIFY_TOP_MARGIN;

    for (i = 0; i < MAX_TOASTS; i++) {
        if (!toasts[i].active) continue;

        int nx = screen_w - NOTIFY_W - NOTIFY_RIGHT_MARGIN;
        int ny = y;

        /* Shadow (subtle, 1-layer) */
        n_alpha_fill(s, nx + 2, ny + 2, NOTIFY_W, NOTIFY_H, 0x000000, 20);

        /* Background — translucent glass */
        n_rounded_rect(s, nx, ny, NOTIFY_W, NOTIFY_H, 3, COL_N_BG, 220);

        /* Accent line at top */
        n_alpha_fill(s, nx + 3, ny, NOTIFY_W - 6, 1, THEME_COL_ACCENT, 40);

        /* Left accent bar */
        n_alpha_fill(s, nx, ny + 3, 2, NOTIFY_H - 6, THEME_COL_ACCENT, 100);

        /* Title */
        gui_surface_draw_string_n(s, nx + NOTIFY_PAD + 4, ny + 8,
                                  toasts[i].title,
                                  (NOTIFY_W - 2 * NOTIFY_PAD - 4) / GUI_FONT_W,
                                  THEME_COL_ACCENT, 0, 0);

        /* Body */
        gui_surface_draw_string_n(s, nx + NOTIFY_PAD + 4, ny + 8 + GUI_FONT_H + 4,
                                  toasts[i].body,
                                  (NOTIFY_W - 2 * NOTIFY_PAD - 4) / GUI_FONT_W,
                                  COL_N_BODY, 0, 0);

        y += NOTIFY_H + NOTIFY_GAP;
    }
}

/* ================================================================
 *  OSD — On-Screen Display (centred indicator bar)
 * ================================================================ */

#define OSD_W          200
#define OSD_H           48
#define OSD_BAR_H       8
#define OSD_BAR_PAD    12
#define OSD_DURATION_MS 1500

static int  osd_vis;
static char osd_icon;
static char osd_label[32];
static int  osd_level;             /* 0–100 */
static unsigned int osd_expire;

void osd_show(char icon, const char *label, int level)
{
    osd_icon  = icon;
    str_copy(osd_label, label ? label : "", sizeof(osd_label));
    osd_level = level < 0 ? 0 : (level > 100 ? 100 : level);
    osd_expire = timer_get_uptime_ms() + OSD_DURATION_MS;
    osd_vis = 1;
    gui_request_redraw();
}

void osd_tick(void)
{
    if (osd_vis && timer_get_uptime_ms() >= osd_expire) {
        osd_vis = 0;
        gui_request_redraw();
    }
}

int osd_active(void) { return osd_vis; }

void osd_paint(void *surface, int screen_w, int screen_h)
{
    gui_surface_t *s = (gui_surface_t *)surface;
    int ox, oy, bar_w, fill_w;

    if (!osd_vis || !s || !s->pixels) return;

    ox = (screen_w - OSD_W) / 2;
    oy = screen_h * 3 / 4 - OSD_H / 2;

    /* Background pill */
    n_rounded_rect(s, ox, oy, OSD_W, OSD_H, 3, COL_N_BG, 210);

    /* Icon (single char) */
    gui_surface_draw_char(s, ox + OSD_BAR_PAD, oy + 8,
                          (unsigned char)osd_icon, THEME_COL_ACCENT, 0, 0);

    /* Label */
    gui_surface_draw_string(s, ox + OSD_BAR_PAD + GUI_FONT_W + 6, oy + 8,
                            osd_label, THEME_COL_TEXT, 0, 0);

    /* Level bar */
    bar_w = OSD_W - 2 * OSD_BAR_PAD;
    fill_w = bar_w * osd_level / 100;
    n_alpha_fill(s, ox + OSD_BAR_PAD, oy + 28, bar_w, OSD_BAR_H,
                 THEME_COL_SURFACE1, 180);
    if (fill_w > 0)
        n_alpha_fill(s, ox + OSD_BAR_PAD, oy + 28, fill_w, OSD_BAR_H,
                     THEME_COL_ACCENT, 200);
}
