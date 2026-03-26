/*
 * Lyth GUI Compositor
 *
 * Architecture:
 * - Each window owns a surface (pixel buffer). Windows only redraw their
 *   surface when content changes (needs_redraw flag).
 * - The compositor maintains a single backbuffer the size of the screen.
 * - A dirty rect list tracks which screen regions need recompositing.
 * - On each frame: (1) re-render dirty windows into their surfaces,
 *   (2) composite only dirty regions into backbuffer, (3) present only
 *   dirty scanlines to the real framebuffer.
 * - The cursor is drawn/erased with save-under, invalidating only its
 *   bounding rect.
 * - Frame pacing targets 60 Hz; if nothing is dirty, the compositor sleeps.
 */

#include "compositor.h"
#include "window.h"
#include "desktop.h"
#include "cursor.h"
#include "theme.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "input.h"
#include "mouse.h"
#include "physmem.h"
#include "string.h"
#include "timer.h"
#include "usb_hid.h"
#include "task.h"
#include "e1000.h"
#include "notify.h"
#include "widgets.h"
#include "renderer.h"

/* ---- VGA port I/O for VBlank detection ---- */
static inline uint8_t compositor_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

/*
 * vblank_wait: block until vertical retrace begins.
 *
 * Polls VGA Input Status Register 1 (port 0x3DA).  Bit 3 is set during
 * the vertical retrace interval (~1.3 ms out of every ~16.7 ms at 60 Hz).
 *
 * To avoid hanging forever if the hardware doesn't emulate retrace,
 * a timeout (in microseconds) is enforced via HPET.
 */
static void vblank_wait(void)
{
    unsigned int t0 = timer_get_monotonic_us();
    /* Phase 1: if already in retrace, wait for it to end */
    while (compositor_inb(0x3DA) & 0x08) {
        if (timer_get_monotonic_us() - t0 > 18000) return; /* ~1 frame timeout */
    }
    /* Phase 2: wait for retrace to start */
    while (!(compositor_inb(0x3DA) & 0x08)) {
        if (timer_get_monotonic_us() - t0 > 18000) return;
    }
}

/* ---- screen state ---- */
static int scr_w, scr_h;
static uint32_t scr_pitch;

/* ---- double buffer ---- */
static uint32_t *backbuffer;
static uint32_t backbuffer_phys;
static uint32_t backbuffer_size;
static gui_surface_t bb_surf; /* wrapper so surface ops work on backbuffer */

/* ---- background cache for fast drag ---- */
static uint32_t *bg_buffer;
static uint32_t bg_buffer_phys;
static int bg_valid; /* 1 if bg_buffer contains a valid snapshot */

/* ---- dirty rect list ---- */
static gui_dirty_rect_t dirty_list[GUI_MAX_DIRTY];
static int dirty_count;

/* shorthand: window is visible AND on current workspace (or sticky) */
#define WIN_ON_WS(w) (((w)->flags & GUI_WIN_VISIBLE) && \
                      !((w)->flags & GUI_WIN_MINIMIZED) && \
                      gui_window_on_current_ws(w))

/* ---- state ---- */
static volatile int gui_running;
static int mouse_x, mouse_y;
static volatile int need_compose;

/* ---- double-click detection ---- */
#define DBLCLICK_MS   400  /* max interval between clicks */
#define DBLCLICK_DIST 6    /* max pixel drift between clicks */
static unsigned int dblclick_last_ms;
static int dblclick_last_x, dblclick_last_y;

/* ---- deferred drag state ---- */
static int drag_pending;           /* 1 if drag position needs applying at frame time */
static int drag_old_x, drag_old_y; /* position before this frame's drag */
static int drag_new_x, drag_new_y; /* desired position for this frame */
static int drag_win_w, drag_win_h;

/* ---- snap preview state ---- */
#define SNAP_NONE  0
#define SNAP_LEFT  1
#define SNAP_RIGHT 2
#define SNAP_FULL  3
static int snap_zone;              /* current snap zone during drag */
static int snap_prev_zone;         /* previous frame's snap zone (for dirty) */

/* ---- metrics ---- */
static gui_metrics_t metrics;
static unsigned int present_count;
static unsigned int fps_timer_ms;
static unsigned int frame_time_acc;
static unsigned int frame_time_samples;
static unsigned int frame_time_max_this_sec;
static unsigned int loop_total_count;  /* total main-loop iters this second */
static unsigned int loop_busy_count;   /* iters that composed a frame */

/* ---- perf overlay ---- */
static int perf_overlay;  /* 1 = show FPS/frametime overlay (toggle with F3) */
static int dirty_debug;   /* 1 = flash dirty rect borders (toggle with F4) */

/* frame time history for graph */
#define FT_HIST_LEN 60
static unsigned int ft_hist[FT_HIST_LEN];
static int ft_hist_idx;

/* ---- alt-tab switcher overlay ---- */
#define ALTTAB_THUMB_W    120
#define ALTTAB_THUMB_H     80
#define ALTTAB_PAD         12
#define ALTTAB_MAX_ITEMS   12

static int alttab_active;            /* 1 = switcher visible */
static int alttab_sel;               /* selected index into alttab_wins[] */
static int alttab_count;             /* number of switchable windows */
static gui_window_t *alttab_wins[ALTTAB_MAX_ITEMS];

/* ---- overview / exposé mode ---- */
#define OVERVIEW_MAX     16
#define OVERVIEW_PAD     24
#define OVERVIEW_TITLE_H 20

static int overview_active;
static int overview_count;
static int overview_hover;           /* hovered thumbnail index (-1 = none) */
static gui_window_t *overview_wins[OVERVIEW_MAX];
/* computed layout per thumbnail */
static int overview_tx[OVERVIEW_MAX], overview_ty[OVERVIEW_MAX];
static int overview_tw[OVERVIEW_MAX], overview_th[OVERVIEW_MAX];

#define PERF_OVL_X 8
#define PERF_OVL_Y 42
#define PERF_OVL_W 170
#define PERF_OVL_LINE (THEME_FONT_H + 2)
#define PERF_OVL_GRAPH_H 40
#define PERF_OVL_LINES 13  /* FPS, Avg, Max, P95, Dirty, Win, Render, Compose, Present, Resolution, GPU, Idle, VRAM */
#define PERF_OVL_TOTAL_H (PERF_OVL_LINE * PERF_OVL_LINES + 8 + PERF_OVL_GRAPH_H + 4)

static unsigned int dropped_frames;  /* frames exceeding 16.7ms budget */

static void paint_perf_overlay(gui_surface_t *s)
{
    char buf[48];
    int y = PERF_OVL_Y;
    uint32_t fg = 0x00FF88;   /* green text */
    uint32_t bg = 0x000000;
    uint32_t dim = 0x66AA88;  /* dimmer green for phase lines */

    if (!perf_overlay) return;

    /* semi-transparent black background */
    gpu_set_target(g_gpu.backbuffer);
    gpu_fill_rect_alpha(PERF_OVL_X - 4, y - 4, PERF_OVL_W, PERF_OVL_TOTAL_H, bg, 180);

    /* FPS + dropped */
    buf[0] = 'F'; buf[1] = 'P'; buf[2] = 'S'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.fps, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = ' '; buf[l+1] = 'd'; buf[l+2] = ':';
      uint_to_str(dropped_frames, buf + l + 3, 10); }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    y += PERF_OVL_LINE;

    /* Frame time avg (us) */
    buf[0] = 'A'; buf[1] = 'v'; buf[2] = 'g'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.frame_time_avg, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    y += PERF_OVL_LINE;

    /* Frame time max (us) */
    buf[0] = 'M'; buf[1] = 'a'; buf[2] = 'x'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.frame_time_max, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    y += PERF_OVL_LINE;

    /* P95 frametime from history */
    {
        unsigned int sorted[FT_HIST_LEN];
        int cnt = 0, i, j;
        unsigned int p95_val = 0;
        for (i = 0; i < FT_HIST_LEN; i++)
            if (ft_hist[i] > 0) sorted[cnt++] = ft_hist[i];
        /* simple insertion sort — only 60 elements */
        for (i = 1; i < cnt; i++) {
            unsigned int key = sorted[i];
            j = i - 1;
            while (j >= 0 && sorted[j] > key) {
                sorted[j + 1] = sorted[j];
                j--;
            }
            sorted[j + 1] = key;
        }
        if (cnt > 0) p95_val = sorted[(cnt * 95) / 100];
        buf[0] = 'P'; buf[1] = '9'; buf[2] = '5'; buf[3] = ':'; buf[4] = ' ';
        uint_to_str(p95_val, buf + 5, 10);
        { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
        gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    }
    y += PERF_OVL_LINE;

    /* Dirty rects */
    buf[0] = 'D'; buf[1] = 'i'; buf[2] = 'r'; buf[3] = 't'; buf[4] = 'y';
    buf[5] = ':'; buf[6] = ' ';
    uint_to_str(metrics.dirty_count, buf + 7, 10);
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    y += PERF_OVL_LINE;

    /* Windows redrawn this frame */
    buf[0] = 'W'; buf[1] = 'i'; buf[2] = 'n'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.windows_redrawn, buf + 5, 10);
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, fg, 0, 0);
    y += PERF_OVL_LINE;

    /* Phase: Render */
    buf[0] = 'R'; buf[1] = 'n'; buf[2] = 'd'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.render_us, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, dim, 0, 0);
    y += PERF_OVL_LINE;

    /* Phase: Compose */
    buf[0] = 'C'; buf[1] = 'm'; buf[2] = 'p'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.compose_us, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, dim, 0, 0);
    y += PERF_OVL_LINE;

    /* Phase: Present */
    buf[0] = 'P'; buf[1] = 'r'; buf[2] = 's'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.present_us, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'u'; buf[l+1] = 's'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, dim, 0, 0);
    y += PERF_OVL_LINE;

    /* Resolution */
    uint_to_str((unsigned int)scr_w, buf, 10);
    { int l = (int)strlen(buf); buf[l] = 'x'; uint_to_str((unsigned int)scr_h, buf + l + 1, 10); }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, 0x88AACC, 0, 0);
    y += PERF_OVL_LINE;

    /* GPU upload (Kpx) */
    buf[0] = 'G'; buf[1] = 'P'; buf[2] = 'U'; buf[3] = ':'; buf[4] = ' ';
    uint_to_str(metrics.gpu_upload_px / 1000, buf + 5, 10);
    { int l = (int)strlen(buf); buf[l] = 'K'; buf[l+1] = 'p'; buf[l+2] = 'x'; buf[l+3] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, 0x88AACC, 0, 0);
    y += PERF_OVL_LINE;

    /* CPU idle % */
    buf[0] = 'I'; buf[1] = 'd'; buf[2] = 'l'; buf[3] = 'e'; buf[4] = ':'; buf[5] = ' ';
    uint_to_str(metrics.idle_pct, buf + 6, 10);
    { int l = (int)strlen(buf); buf[l] = '%'; buf[l+1] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, metrics.idle_pct > 50 ? fg : 0xFF4444, 0, 0);
    y += PERF_OVL_LINE;

    /* VRAM usage (KB) */
    buf[0] = 'V'; buf[1] = 'R'; buf[2] = 'A'; buf[3] = 'M'; buf[4] = ':'; buf[5] = ' ';
    uint_to_str(metrics.vram_kb, buf + 6, 10);
    { int l = (int)strlen(buf); buf[l] = 'K'; buf[l+1] = 'B'; buf[l+2] = 0; }
    gui_surface_draw_string(s, PERF_OVL_X, y, buf, 0x88AACC, 0, 0);
    y += PERF_OVL_LINE;

    /* Frame time bar graph */
    {
        int gx = PERF_OVL_X;
        int gy = y + 2;
        int bar_w = (PERF_OVL_W - 8) / FT_HIST_LEN;
        int gi;
        unsigned int max_ft = 16667; /* 60fps baseline = ~16.7ms */
        if (bar_w < 1) bar_w = 1;

        /* find graph max for scaling (at least 16.7ms) */
        for (gi = 0; gi < FT_HIST_LEN; gi++) {
            if (ft_hist[gi] > max_ft) max_ft = ft_hist[gi];
        }

        /* 16.7ms reference line (green dashed) */
        {
            int ref_y = gy + PERF_OVL_GRAPH_H -
                        (int)((16667UL * (unsigned int)PERF_OVL_GRAPH_H) / max_ft);
            if (ref_y >= gy && ref_y < gy + PERF_OVL_GRAPH_H)
                gpu_fill_rect_alpha(gx, ref_y, FT_HIST_LEN * bar_w, 1, 0x00FF88, 80);
        }

        for (gi = 0; gi < FT_HIST_LEN; gi++) {
            int idx = (ft_hist_idx + gi) % FT_HIST_LEN;
            unsigned int ft_val = ft_hist[idx];
            int bar_h;
            uint32_t bar_col;

            if (ft_val == 0) continue;
            bar_h = (int)((ft_val * (unsigned int)PERF_OVL_GRAPH_H) / max_ft);
            if (bar_h < 1) bar_h = 1;
            if (bar_h > PERF_OVL_GRAPH_H) bar_h = PERF_OVL_GRAPH_H;

            /* green if under 16.7ms, yellow 16.7-33ms, red over 33ms */
            if (ft_val <= 16667) bar_col = 0x00FF88;
            else if (ft_val <= 33333) bar_col = 0xFFDD44;
            else bar_col = 0xFF4444;

            gpu_fill_rect_alpha(gx + gi * bar_w,
                                gy + PERF_OVL_GRAPH_H - bar_h,
                                bar_w > 1 ? bar_w - 1 : 1, bar_h,
                                bar_col, 200);
        }
    }
}

/* paint alt-tab switcher overlay centered on screen */
static void paint_alttab_overlay(gui_surface_t *s)
{
    int total_w, total_h, ox, oy, i;
    if (!alttab_active || alttab_count < 1) return;

    total_w = alttab_count * (ALTTAB_THUMB_W + ALTTAB_PAD) + ALTTAB_PAD;
    total_h = ALTTAB_THUMB_H + ALTTAB_PAD * 2 + THEME_FONT_H + 8;
    ox = (scr_w - total_w) / 2;
    oy = (scr_h - total_h) / 2;

    /* dark semi-transparent backdrop */
    gpu_set_target(g_gpu.backbuffer);
    gpu_fill_rect_alpha(ox, oy, total_w, total_h, 0x181825, 220);

    for (i = 0; i < alttab_count; i++) {
        gui_window_t *w = alttab_wins[i];
        int tx = ox + ALTTAB_PAD + i * (ALTTAB_THUMB_W + ALTTAB_PAD);
        int ty = oy + ALTTAB_PAD;

        /* selection highlight */
        if (i == alttab_sel) {
            gpu_fill_rect_alpha(tx - 3, ty - 3,
                                ALTTAB_THUMB_W + 6, ALTTAB_THUMB_H + 6,
                                THEME_COL_ACCENT, 120);
        }

        /* thumbnail: scale window surface into the slot */
        if (w && w->surface.pixels && w->surface.width > 0 && w->surface.height > 0) {
            gpu_texture_t thumb_tex;
            gpu_texture_wrap(&thumb_tex, w->surface.pixels,
                             w->surface.width, w->surface.height,
                             w->surface.stride);
            gpu_set_target(g_gpu.backbuffer);
            gpu_blit_scaled(tx, ty, ALTTAB_THUMB_W, ALTTAB_THUMB_H,
                            &thumb_tex, 0, 0,
                            w->surface.width, w->surface.height);
        } else {
            /* no surface: grey placeholder */
            gpu_set_target(g_gpu.backbuffer);
            gpu_fill_rect_alpha(tx, ty, ALTTAB_THUMB_W, ALTTAB_THUMB_H, 0x45475A, 200);
        }

        /* title below thumbnail */
        if (w) {
            int tw = (int)strlen(w->title) * THEME_FONT_W;
            int ttx = tx + (ALTTAB_THUMB_W - tw) / 2;
            if (ttx < tx) ttx = tx;
            gui_surface_draw_string_n(s, ttx, ty + ALTTAB_THUMB_H + 4,
                                      w->title, ALTTAB_THUMB_W / THEME_FONT_W,
                                      i == alttab_sel ? THEME_COL_ACCENT : THEME_COL_TEXT,
                                      0, 0);
        }
    }
}

/* ---- Exposé / Overview mode ---- */

static void overview_build(void)
{
    int i, count = gui_window_count();
    overview_count = 0;
    for (i = 0; i < count && overview_count < OVERVIEW_MAX; i++) {
        gui_window_t *w = gui_window_get(i);
        if (w && WIN_ON_WS(w))
            overview_wins[overview_count++] = w;
    }
    if (overview_count < 1) { overview_active = 0; return; }

    /* compute grid layout */
    {
        int cols, rows, c;
        int usable_w = scr_w - 2 * OVERVIEW_PAD;
        int usable_h = scr_h - 2 * OVERVIEW_PAD;
        int cell_w, cell_h;

        /* pick grid dimensions */
        for (cols = 1; cols * cols < overview_count; cols++) {}
        rows = (overview_count + cols - 1) / cols;

        cell_w = usable_w / cols;
        cell_h = usable_h / rows;

        for (c = 0; c < overview_count; c++) {
            int col = c % cols;
            int row = c / cols;
            gui_window_t *w = overview_wins[c];
            int max_tw = cell_w - 16;
            int max_th = cell_h - OVERVIEW_TITLE_H - 16;
            int tw, th;

            if (max_tw < 40) max_tw = 40;
            if (max_th < 30) max_th = 30;

            /* aspect-fit */
            tw = max_tw;
            th = w->width > 0 ? (tw * w->height / w->width) : max_th;
            if (th > max_th) {
                th = max_th;
                tw = w->height > 0 ? (th * w->width / w->height) : max_tw;
            }
            if (tw > max_tw) tw = max_tw;

            overview_tx[c] = OVERVIEW_PAD + col * cell_w + (cell_w - tw) / 2;
            overview_ty[c] = OVERVIEW_PAD + row * cell_h + (cell_h - th - OVERVIEW_TITLE_H) / 2;
            overview_tw[c] = tw;
            overview_th[c] = th;
        }
    }
    overview_hover = -1;
    overview_active = 1;
}

static void paint_overview(gui_surface_t *s)
{
    int i;
    if (!overview_active || overview_count < 1) return;

    /* full-screen dim */
    gpu_set_target(g_gpu.backbuffer);
    gpu_fill_rect_alpha(0, 0, scr_w, scr_h, 0x000000, 140);

    for (i = 0; i < overview_count; i++) {
        gui_window_t *w = overview_wins[i];
        int tx = overview_tx[i];
        int ty = overview_ty[i];
        int tw = overview_tw[i];
        int th = overview_th[i];

        /* hover highlight */
        if (i == overview_hover) {
            gpu_set_target(g_gpu.backbuffer);
            gpu_fill_rect_alpha(tx - 4, ty - 4, tw + 8, th + 8,
                                THEME_COL_ACCENT, 80);
        }

        /* thumbnail */
        if (w && w->surface.pixels && w->surface.width > 0 && w->surface.height > 0) {
            gpu_texture_t tex;
            gpu_texture_wrap(&tex, w->surface.pixels,
                             w->surface.width, w->surface.height,
                             w->surface.stride);
            gpu_set_target(g_gpu.backbuffer);
            gpu_blit_scaled(tx, ty, tw, th, &tex, 0, 0,
                            w->surface.width, w->surface.height);
        } else {
            gpu_set_target(g_gpu.backbuffer);
            gpu_fill_rect_alpha(tx, ty, tw, th, THEME_COL_SURFACE0, 200);
        }

        /* 1px border */
        {
            int bx, by;
            for (bx = tx; bx < tx + tw; bx++) {
                if (ty >= 0 && ty < scr_h && bx >= 0 && bx < scr_w)
                    s->pixels[ty * s->stride + bx] = THEME_COL_BORDER;
                if (ty + th - 1 >= 0 && ty + th - 1 < scr_h && bx >= 0 && bx < scr_w)
                    s->pixels[(ty + th - 1) * s->stride + bx] = THEME_COL_BORDER;
            }
            for (by = ty; by < ty + th; by++) {
                if (by >= 0 && by < scr_h && tx >= 0 && tx < scr_w)
                    s->pixels[by * s->stride + tx] = THEME_COL_BORDER;
                if (by >= 0 && by < scr_h && tx + tw - 1 >= 0 && tx + tw - 1 < scr_w)
                    s->pixels[by * s->stride + (tx + tw - 1)] = THEME_COL_BORDER;
            }
        }

        /* title below */
        if (w) {
            int tlen = (int)strlen(w->title);
            int max_chars = tw / THEME_FONT_W;
            int tw_text = tlen * THEME_FONT_W;
            int ttx = tx + (tw - tw_text) / 2;
            if (ttx < tx) ttx = tx;
            gui_surface_draw_string_n(s, ttx, ty + th + 4,
                                      w->title, max_chars,
                                      i == overview_hover ? THEME_COL_ACCENT : THEME_COL_TEXT,
                                      0, 0);
        }
    }

    /* Label */
    {
        const char *lbl = "Overview (F5 to close, click to select)";
        int lw = str_length(lbl) * THEME_FONT_W;
        gui_surface_draw_string(s, (scr_w - lw) / 2, scr_h - 30, lbl,
                                THEME_COL_DIM, 0, 0);
    }
}

/* ---- frame pacing ---- */
#define FRAME_INTERVAL_MS 16U /* ~60 fps */

/* ==================================================================
 *  Dirty rect management
 * ================================================================== */

static gui_dirty_rect_t rect_clip(gui_dirty_rect_t r)
{
    int x1, y1;
    if (r.x < 0)
    {
        r.w += r.x;
        r.x = 0;
    }
    if (r.y < 0)
    {
        r.h += r.y;
        r.y = 0;
    }
    x1 = r.x + r.w;
    y1 = r.y + r.h;
    if (x1 > scr_w)
        r.w = scr_w - r.x;
    if (y1 > scr_h)
        r.h = scr_h - r.y;
    if (r.w < 0)
        r.w = 0;
    if (r.h < 0)
        r.h = 0;
    return r;
}

static int rects_overlap(const gui_dirty_rect_t *a, const gui_dirty_rect_t *b)
{
    return !(a->x + a->w <= b->x || b->x + b->w <= a->x ||
             a->y + a->h <= b->y || b->y + b->h <= a->y);
}

static gui_dirty_rect_t rect_union(const gui_dirty_rect_t *a, const gui_dirty_rect_t *b)
{
    gui_dirty_rect_t r;
    int ax1 = a->x + a->w, ay1 = a->y + a->h;
    int bx1 = b->x + b->w, by1 = b->y + b->h;
    r.x = a->x < b->x ? a->x : b->x;
    r.y = a->y < b->y ? a->y : b->y;
    r.w = (ax1 > bx1 ? ax1 : bx1) - r.x;
    r.h = (ay1 > by1 ? ay1 : by1) - r.y;
    return r;
}

void gui_dirty_add(int x, int y, int w, int h)
{
    gui_dirty_rect_t r;
    int i;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    r = rect_clip(r);
    if (r.w <= 0 || r.h <= 0)
        return;

    /* try to merge with existing rect */
    for (i = 0; i < dirty_count; i++)
    {
        if (rects_overlap(&dirty_list[i], &r))
        {
            dirty_list[i] = rect_union(&dirty_list[i], &r);
            need_compose = 1;
            return;
        }
    }

    if (dirty_count < GUI_MAX_DIRTY)
    {
        dirty_list[dirty_count++] = r;
    }
    else
    {
        /* overflow: merge into full screen */
        dirty_list[0].x = 0;
        dirty_list[0].y = 0;
        dirty_list[0].w = scr_w;
        dirty_list[0].h = scr_h;
        dirty_count = 1;
    }
    need_compose = 1;
}

void gui_dirty_add_rect(const gui_dirty_rect_t *r)
{
    if (r)
        gui_dirty_add(r->x, r->y, r->w, r->h);
}

void gui_dirty_screen(void)
{
    dirty_list[0].x = 0;
    dirty_list[0].y = 0;
    dirty_list[0].w = scr_w;
    dirty_list[0].h = scr_h;
    dirty_count = 1;
    need_compose = 1;
}

/* ==================================================================
 *  Compositing: blend windows into backbuffer for dirty regions
 * ================================================================== */

static void sort_windows(gui_window_t **sorted, int count)
{
    int i, j;
    for (i = 1; i < count; i++)
    {
        gui_window_t *key = sorted[i];
        j = i - 1;
        while (j >= 0 && sorted[j]->z_order > key->z_order)
        {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
}

/* Check if a window's rect intersects a dirty rect */
static int win_intersects_dirty(gui_window_t *w, const gui_dirty_rect_t *d)
{
    return !(w->x + w->width <= d->x || d->x + d->w <= w->x ||
             w->y + w->height <= d->y || d->y + d->h <= w->y);
}

/* Composite a single dirty region: paint desktop bg then overlay windows */
static void compose_region(gui_dirty_rect_t *dr, gui_window_t **sorted, int wcount)
{
    int i, row;
    int dx0 = dr->x, dy0 = dr->y;
    int dx1 = dr->x + dr->w, dy1 = dr->y + dr->h;

    /* 1. Paint desktop background into this region of backbuffer */
    desktop_paint_region(&bb_surf, dx0, dy0, dx1, dy1);

    /* 2. Draw window shadows via renderer */
    gpu_set_target(g_gpu.backbuffer);
    gpu_set_clip(dx0, dy0, dx1 - dx0, dy1 - dy0);
    for (i = 0; i < wcount; i++)
    {
        gui_window_t *w = sorted[i];

        if (!WIN_ON_WS(w))
            continue;
        if (!w->surface.pixels)
            continue;

        /* Draw multi-layer soft drop shadow (skip during fade) */
        if (w->alpha == 255) {
            static const int sh_off[]    = { THEME_SHADOW_OFF0,    THEME_SHADOW_OFF1,    THEME_SHADOW_OFF2 };
            static const int sh_spread[] = { THEME_SHADOW_SPREAD0, THEME_SHADOW_SPREAD1, THEME_SHADOW_SPREAD2 };
            static const int sh_alpha[]  = { THEME_SHADOW_ALPHA0,  THEME_SHADOW_ALPHA1,  THEME_SHADOW_ALPHA2 };
            int layer;
            for (layer = THEME_SHADOW_LAYERS - 1; layer >= 0; layer--) {
                int a = sh_alpha[layer] * theme.shadow_alpha_mul / 100;
                if (a > 255) a = 255;
                gpu_shadow(w->x, w->y, w->width, w->height,
                           sh_off[layer], sh_spread[layer], a);
            }
        }
    }
    gpu_clear_clip();

    /* 3. Blit each window that overlaps this dirty rect (via renderer) */
    gpu_set_target(g_gpu.backbuffer);
    gpu_set_clip(dx0, dy0, dx1 - dx0, dy1 - dy0);
    for (i = 0; i < wcount; i++)
    {
        gui_window_t *w = sorted[i];
        int wx0, wy0, wx1, wy1;
        int sx, sy, dstx, dsty, bw, bh;

        if (!WIN_ON_WS(w))
            continue;
        if (!win_intersects_dirty(w, dr))
            continue;
        if (!w->surface.pixels)
            continue;

        /* intersect window rect with dirty rect */
        wx0 = w->x > dx0 ? w->x : dx0;
        wy0 = w->y > dy0 ? w->y : dy0;
        wx1 = (w->x + w->width) < dx1 ? (w->x + w->width) : dx1;
        wy1 = (w->y + w->height) < dy1 ? (w->y + w->height) : dy1;
        if (wx0 >= wx1 || wy0 >= wy1)
            continue;

        sx = wx0 - w->x;
        sy = wy0 - w->y;
        dstx = wx0;
        dsty = wy0;
        bw = wx1 - wx0;
        bh = wy1 - wy0;

        /* wrap window surface as renderer texture */
        {
            gpu_texture_t win_tex;
            int cr = (w->flags & GUI_WIN_NO_DECOR) ? 0 : THEME_WIN_RADIUS;
            gpu_texture_wrap(&win_tex, w->surface.pixels,
                             w->surface.width, w->surface.height,
                             w->surface.stride);

            if (cr == 0) {
                /* no corners — single blit through renderer */
                if (w->alpha < 255)
                    gpu_blit_alpha(dstx, dsty, &win_tex, sx, sy, bw, bh, w->alpha);
                else
                    gpu_blit(dstx, dsty, &win_tex, sx, sy, bw, bh);
            } else {
                /* rounded corners: single full blit, then patch corner pixels
                 * from desktop background.  Much faster than per-row blits.  */
                if (w->alpha < 255)
                    gpu_blit_alpha(dstx, dsty, &win_tex, sx, sy, bw, bh, w->alpha);
                else
                    gpu_blit(dstx, dsty, &win_tex, sx, sy, bw, bh);

                /* Corner inset table derived from THEME_WIN_RADIUS:
                 * row 0→R px, row 1→R-1 px, ... row R-1→1 px */
                {
                    int ci;
                    for (ci = 0; ci < THEME_WIN_RADIUS; ci++) {
                        int ins = THEME_WIN_RADIUS - ci;
                        int top_src = ci;                     /* row in window */
                        int bot_src = w->surface.height - 1 - ci;

                        /* top-left corner */
                        if (top_src >= sy && top_src < sy + bh && sx < ins) {
                            int py = dsty + (top_src - sy);
                            int pw = ins - sx;
                            if (pw > bw) pw = bw;
                            if (py >= 0 && py < scr_h && dstx >= 0 && pw > 0)
                                desktop_paint_region(&bb_surf, dstx, py, dstx + pw, py + 1);
                        }
                        /* top-right corner */
                        if (top_src >= sy && top_src < sy + bh && sx + bw > w->width - ins) {
                            int py = dsty + (top_src - sy);
                            int rx = w->width - ins;
                            int lx = rx > sx ? rx - sx : 0;
                            int px = dstx + lx;
                            int pw = bw - lx;
                            if (pw > 0 && py >= 0 && py < scr_h && px < scr_w)
                                desktop_paint_region(&bb_surf, px, py, px + pw, py + 1);
                        }
                        /* bottom-left corner */
                        if (bot_src >= sy && bot_src < sy + bh && sx < ins) {
                            int py = dsty + (bot_src - sy);
                            int pw = ins - sx;
                            if (pw > bw) pw = bw;
                            if (py >= 0 && py < scr_h && dstx >= 0 && pw > 0)
                                desktop_paint_region(&bb_surf, dstx, py, dstx + pw, py + 1);
                        }
                        /* bottom-right corner */
                        if (bot_src >= sy && bot_src < sy + bh && sx + bw > w->width - ins) {
                            int py = dsty + (bot_src - sy);
                            int rx = w->width - ins;
                            int lx = rx > sx ? rx - sx : 0;
                            int px = dstx + lx;
                            int pw = bw - lx;
                            if (pw > 0 && py >= 0 && py < scr_h && px < scr_w)
                                desktop_paint_region(&bb_surf, px, py, px + pw, py + 1);
                        }
                    }
                }
            }
        }
    }
    gpu_clear_clip();

    /* 4. Paint overlays on top of everything (launcher / start menu) */
    desktop_paint_overlays(&bb_surf, dx0, dy0, dx1, dy1);

    /* 5. Paint notification toasts (only if overlapping this dirty rect) */
    if (notify_count() > 0) {
        int nrx = scr_w - 260 - 10;  /* NOTIFY_W + NOTIFY_RIGHT_MARGIN */
        int nry = 10;                /* NOTIFY_TOP_MARGIN */
        int nrh = notify_count() * 60; /* NOTIFY_H + NOTIFY_GAP per toast */
        if (!(nrx + 260 <= dx0 || dx1 <= nrx ||
              nry + nrh <= dy0 || dy1 <= nry))
            notify_paint(&bb_surf, scr_w);
    }

    /* 5b. OSD indicator bar (centred, lower third) */
    if (osd_active()) {
        int osd_x = (scr_w - 200) / 2;
        int osd_y = scr_h * 3 / 4 - 24;
        if (!(osd_x + 200 <= dx0 || dx1 <= osd_x ||
              osd_y + 48 <= dy0 || dy1 <= osd_y))
            osd_paint(&bb_surf, scr_w, scr_h);
    }

    /* 6. Perf overlay (if enabled) */
    if (perf_overlay) {
        int pox = PERF_OVL_X - 4, poy = PERF_OVL_Y - 4;
        int poh = PERF_OVL_LINE * 5 + 8;
        if (!(pox + PERF_OVL_W <= dx0 || dx1 <= pox ||
              poy + poh <= dy0 || dy1 <= poy))
            paint_perf_overlay(&bb_surf);
    }

    /* 7. Alt-Tab switcher overlay */
    if (alttab_active && alttab_count > 0)
        paint_alttab_overlay(&bb_surf);

    /* 8. Exposé / overview overlay */
    if (overview_active && overview_count > 0)
        paint_overview(&bb_surf);

    /* track pixels copied */
    metrics.pixels_copied += (unsigned int)(dr->w * dr->h);
}

/* Full compose pass: re-render dirty windows, then compose dirty regions */
static void compose_frame(void)
{
    int i, wcount;
    gui_window_t *sorted[GUI_MAX_WINDOWS];
    unsigned int t0, t1, t2;

    if (dirty_count == 0)
        return;

    t0 = timer_get_monotonic_us();

    /* Phase 1: re-render windows whose content changed AND are visible
     * in at least one dirty region (skip off-screen repaints).
     * After repainting, ensure the full window rect is dirty so that
     * all of its surface gets composed — not just the small rect that
     * triggered the repaint. We collect repainted rects in a temp list
     * and merge them after the loop to avoid mutating dirty_list while
     * iterating it. */
    {
        gui_dirty_rect_t repaint_rects[GUI_MAX_WINDOWS];
        int repaint_count = 0;
        int win_redrawn = 0;

        wcount = gui_window_count();
        for (i = 0; i < wcount; i++)
        {
            gui_window_t *w = gui_window_get(i);
            if (w && w->needs_redraw && (w->flags & GUI_WIN_VISIBLE) &&
                gui_window_on_current_ws(w))
            {
                int j, dominated = 0;
                for (j = 0; j < dirty_count; j++)
                {
                    if (win_intersects_dirty(w, &dirty_list[j]))
                    {
                        dominated = 1;
                        break;
                    }
                }
                if (dominated)
                {
                    if (w->on_paint)
                    {
                        w->on_paint(w);
                    }
                    /* draw widget-kit widgets on top of app content */
                    if (w->widget_count > 0)
                        wid_draw_all(w);
                    w->needs_redraw = 0;
                    w->redraw_count++;
                    win_redrawn++;
                    /* queue full window rect for dirty merge */
                    if (repaint_count < GUI_MAX_WINDOWS)
                    {
                        repaint_rects[repaint_count].x = w->x;
                        repaint_rects[repaint_count].y = w->y;
                        repaint_rects[repaint_count].w = w->width;
                        repaint_rects[repaint_count].h = w->height;
                        repaint_count++;
                    }
                }
            }
        }
        metrics.windows_redrawn = (unsigned int)win_redrawn;

        /* merge repainted window rects into dirty list */
        for (i = 0; i < repaint_count; i++)
        {
            gui_dirty_add(repaint_rects[i].x, repaint_rects[i].y,
                          repaint_rects[i].w, repaint_rects[i].h);
        }
    }

    t1 = timer_get_monotonic_us();
    metrics.render_us = t1 - t0;

    /* Phase 2: gather and sort visible windows */
    for (i = 0; i < wcount; i++)
    {
        sorted[i] = gui_window_get(i);
    }
    sort_windows(sorted, wcount);

    /* Phase 3: composite each dirty region */
    metrics.dirty_count = (unsigned int)dirty_count;
    metrics.pixels_copied = 0;
    for (i = 0; i < dirty_count; i++)
    {
        compose_region(&dirty_list[i], sorted, wcount);
    }

    t2 = timer_get_monotonic_us();
    metrics.compose_us = t2 - t1;
}

/* ==================================================================
 *  Fast drag path (ported from compositor.c.bak)
 *
 *  Renders desktop + all windows EXCEPT the dragging one into bg_buffer
 *  once at the start of a drag.  Each subsequent drag frame just copies
 *  the cached background, blits the dragging window's surface on top,
 *  draws the cursor, and pushes two rectangles (old + new) to the
 *  framebuffer.  Much faster than the dirty-rect compose path.
 * ================================================================== */

/*
 * rebuild_bg: build bg_buffer (scene without the dragged window).
 *
 * Copy the backbuffer (which has the correct composed scene), then
 * repaint ONLY the dragged window's rect with desktop + other windows
 * to erase it from the background cache.
 */
static void rebuild_bg(gui_window_t *skip_win)
{
    int i, wcount, row;
    gui_window_t *sorted[GUI_MAX_WINDOWS];
    gui_surface_t bg_surf;
    int wx0, wy0, wx1, wy1;

    if (!bg_buffer || !backbuffer)
        return;

    /* 1. Erase cursor so the copy is clean */
    cursor_erase(&bb_surf);

    /* 2. Copy entire backbuffer → bg_buffer.
     *    A single memcpy of ~3MB is very fast on any modern CPU. */
    memcpy(bg_buffer, backbuffer, (size_t)scr_w * scr_h * 4);

    /* 3. Restore cursor */
    cursor_draw(&bb_surf);

    if (!skip_win)
    {
        bg_valid = 1;
        return;
    }

    /* 4. Compute the dragged window's on-screen rect + shadow (clipped) */
    wx0 = skip_win->x - THEME_SHADOW_EXTENT;
    wy0 = skip_win->y - THEME_SHADOW_EXTENT;
    wx1 = skip_win->x + skip_win->width + THEME_SHADOW_EXTENT;
    wy1 = skip_win->y + skip_win->height + THEME_SHADOW_EXTENT;
    if (wx0 < 0) wx0 = 0;
    if (wy0 < 0) wy0 = 0;
    if (wx1 > scr_w) wx1 = scr_w;
    if (wy1 > scr_h) wy1 = scr_h;
    if (wx0 >= wx1 || wy0 >= wy1)
    {
        bg_valid = 1;
        return;
    }

    /* 5. Repaint ONLY the dragged window's rect in bg_buffer */
    bg_surf.pixels = bg_buffer;
    bg_surf.width = scr_w;
    bg_surf.height = scr_h;
    bg_surf.stride = scr_w;
    bg_surf.alloc_phys = 0;
    bg_surf.alloc_size = 0;

    desktop_paint_region(&bg_surf, wx0, wy0, wx1, wy1);

    wcount = gui_window_count();
    for (i = 0; i < wcount; i++)
    {
        sorted[i] = gui_window_get(i);
    }
    sort_windows(sorted, wcount);

    for (i = 0; i < wcount; i++)
    {
        gui_window_t *w = sorted[i];
        int ox0, oy0, ox1, oy1;
        int sx, sy, dstx, dsty, bw, bh;

        if (w == skip_win)
            continue;
        if (!WIN_ON_WS(w))
            continue;
        if (!w->surface.pixels)
            continue;

        ox0 = w->x > wx0 ? w->x : wx0;
        oy0 = w->y > wy0 ? w->y : wy0;
        ox1 = (w->x + w->width) < wx1 ? (w->x + w->width) : wx1;
        oy1 = (w->y + w->height) < wy1 ? (w->y + w->height) : wy1;
        if (ox0 >= ox1 || oy0 >= oy1)
            continue;

        sx = ox0 - w->x;
        sy = oy0 - w->y;
        dstx = ox0;
        dsty = oy0;
        bw = ox1 - ox0;
        bh = oy1 - oy0;

        for (row = 0; row < bh; row++)
        {
            memcpy(&bg_buffer[(dsty + row) * scr_w + dstx],
                   &w->surface.pixels[(sy + row) * w->surface.stride + sx],
                   (size_t)bw * 4);
        }
    }

    bg_valid = 1;
}

/* (composite_drag removed — logic now inlined in gui_run drag path) */

/* ==================================================================
 *  Present: copy dirty scanlines from backbuffer to framebuffer
 * ================================================================== */

static void present_dirty(void)
{
    unsigned int t0, t1;
    unsigned int upload_px = 0;
    int i;

    t0 = timer_get_monotonic_us();

    /* For virtio-GPU, each present involves a synchronous round-trip.
     * However, unioning far-apart dirty rects wastes bandwidth by
     * transferring many unchanged pixels.  Strategy: union only when
     * the bounding box area is within 3× the sum of individual areas;
     * otherwise present each rect individually.
     * For the SW backend, presents are cheap memcpy — always individual. */
    if (dirty_count <= 1) {
        if (dirty_count == 1) {
            gpu_present(dirty_list[0].x, dirty_list[0].y,
                        dirty_list[0].w, dirty_list[0].h);
            upload_px += (unsigned int)(dirty_list[0].w * dirty_list[0].h);
        }
    } else if (g_gpu.ops && g_gpu.ops->name && g_gpu.ops->name[0] == 'v') {
        /* virtio path: union only if efficient */
        int ux = dirty_list[0].x, uy = dirty_list[0].y;
        int ur = ux + dirty_list[0].w, ub = uy + dirty_list[0].h;
        unsigned int sum_area = 0;
        for (i = 0; i < dirty_count; i++) {
            sum_area += (unsigned int)(dirty_list[i].w * dirty_list[i].h);
            if (dirty_list[i].x < ux) ux = dirty_list[i].x;
            if (dirty_list[i].y < uy) uy = dirty_list[i].y;
            if (dirty_list[i].x + dirty_list[i].w > ur)
                ur = dirty_list[i].x + dirty_list[i].w;
            if (dirty_list[i].y + dirty_list[i].h > ub)
                ub = dirty_list[i].y + dirty_list[i].h;
        }
        {
            unsigned int union_area = (unsigned int)(ur - ux) * (unsigned int)(ub - uy);
            if (union_area <= sum_area * 3) {
                /* union is efficient, single present */
                gpu_present(ux, uy, ur - ux, ub - uy);
                upload_px += union_area;
            } else {
                /* union would waste >3× bandwidth, present individually */
                for (i = 0; i < dirty_count; i++) {
                    gpu_present(dirty_list[i].x, dirty_list[i].y,
                                dirty_list[i].w, dirty_list[i].h);
                    upload_px += (unsigned int)(dirty_list[i].w * dirty_list[i].h);
                }
            }
        }
    } else {
        /* SW path: present each dirty rect individually */
        for (i = 0; i < dirty_count; i++) {
            gpu_present(dirty_list[i].x, dirty_list[i].y,
                        dirty_list[i].w, dirty_list[i].h);
            upload_px += (unsigned int)(dirty_list[i].w * dirty_list[i].h);
        }
    }

    t1 = timer_get_monotonic_us();
    metrics.present_us = t1 - t0;
    metrics.gpu_upload_px = upload_px;
}

/* ==================================================================
 *  Input handling
 * ================================================================== */

static gui_window_t *hit_test_window(int mx, int my)
{
    int i, count;
    gui_window_t *hit = 0;
    count = gui_window_count();
    for (i = 0; i < count; i++)
    {
        gui_window_t *w = gui_window_get(i);
        if (!w || !WIN_ON_WS(w))
            continue;
        if (w->anim_closing || w->anim_minimizing)
            continue;
        if (mx >= w->x && mx < w->x + w->width &&
            my >= w->y && my < w->y + w->height)
        {
            if (!hit || w->z_order > hit->z_order)
                hit = w;
        }
    }
    return hit;
}

static int hit_close_button(gui_window_t *win, int mx, int my)
{
    int cx, cy;
    if (!(win->flags & GUI_WIN_CLOSEABLE))
        return 0;
    if (win->flags & GUI_WIN_NO_DECOR)
        return 0;
    /* Close button is now on the LEFT (traffic-light style), center at X=16 */
    cx = win->x + 16;
    cy = win->y + GUI_TITLEBAR_HEIGHT / 2;
    return (mx >= cx - 8 && mx <= cx + 8 && my >= cy - 8 && my <= cy + 8);
}

static int hit_minimize_button(gui_window_t *win, int mx, int my)
{
    int cx, cy;
    if (win->flags & GUI_WIN_NO_DECOR)
        return 0;
    /* Minimize button center at X=36 (16 + 20) */
    cx = win->x + 36;
    cy = win->y + GUI_TITLEBAR_HEIGHT / 2;
    return (mx >= cx - 8 && mx <= cx + 8 && my >= cy - 8 && my <= cy + 8);
}

static int hit_titlebar(gui_window_t *win, int mx, int my)
{
    if (win->flags & GUI_WIN_NO_DECOR)
        return 0;
    return (mx >= win->x && mx < win->x + win->width &&
            my >= win->y && my < win->y + GUI_TITLEBAR_HEIGHT);
}

/* Returns GUI_RESIZE_NONE / RIGHT / BOTTOM / BR based on hit position */
static int hit_resize_edge(gui_window_t *win, int mx, int my)
{
    int g = GUI_RESIZE_GRAB;
    int right_zone, bottom_zone;
    if (!(win->flags & GUI_WIN_RESIZABLE))
        return GUI_RESIZE_NONE;
    if (win->flags & GUI_WIN_NO_DECOR)
        return GUI_RESIZE_NONE;

    right_zone  = (mx >= win->x + win->width - g && mx < win->x + win->width &&
                   my >= win->y + GUI_TITLEBAR_HEIGHT && my < win->y + win->height);
    bottom_zone = (my >= win->y + win->height - g && my < win->y + win->height &&
                   mx >= win->x && mx < win->x + win->width);

    if (right_zone && bottom_zone)
        return GUI_RESIZE_BR;
    if (right_zone)
        return GUI_RESIZE_RIGHT;
    if (bottom_zone)
        return GUI_RESIZE_BOTTOM;
    return GUI_RESIZE_NONE;
}

static wid_t *hit_widget(gui_window_t *win, int mx, int my)
{
    int cx = gui_window_content_x(win);
    int cy = gui_window_content_y(win);
    int i;
    for (i = 0; i < win->widget_count; i++)
    {
        wid_t *w = &win->widgets[i];
        if (!(w->state & WID_VISIBLE) || !(w->state & WID_ENABLED))
            continue;
        int wx = cx + w->x;
        int wy = cy + w->y;
        if (mx >= wx && mx < wx + w->width &&
            my >= wy && my < wy + w->height)
            return w;
    }
    return 0;
}

static void handle_keyboard(input_event_t *ev)
{
    int i, count;

    /* F3: toggle perf overlay */
    if (ev->type == INPUT_EVENT_F3)
    {
        perf_overlay = !perf_overlay;
        gui_dirty_add(PERF_OVL_X - 4, PERF_OVL_Y - 4,
                      PERF_OVL_W, PERF_OVL_TOTAL_H);
        return;
    }

    /* F4 (no Alt): toggle dirty rect debug overlay */
    if (ev->type == INPUT_EVENT_F4 && !(ev->modifiers & KEY_MOD_ALT))
    {
        dirty_debug = !dirty_debug;
        return;
    }

    /* F5: toggle exposé / overview */
    if (ev->type == INPUT_EVENT_F5)
    {
        if (!overview_active)
            overview_build();
        else
            overview_active = 0;
        gui_dirty_screen();
        return;
    }

    /* Escape: dismiss overview */
    if (overview_active && ev->type == INPUT_EVENT_CHAR && ev->character == 27)
    {
        overview_active = 0;
        gui_dirty_screen();
        return;
    }

    /* Block keyboard input while overview is active */
    if (overview_active) return;

    /* Ctrl+Alt+1/2/3/4: switch workspace */
    if (ev->type == INPUT_EVENT_CHAR &&
        (ev->modifiers & KEY_MOD_CTRL) && (ev->modifiers & KEY_MOD_ALT))
    {
        int ws = -1;
        if (ev->character == '1') ws = 0;
        else if (ev->character == '2') ws = 1;
        else if (ev->character == '3') ws = 2;
        else if (ev->character == '4') ws = 3;
        if (ws >= 0) {
            gui_workspace_switch(ws);
            return;
        }
    }

    /* Alt+F4: close the focused window */
    if (ev->type == INPUT_EVENT_F4 && (ev->modifiers & KEY_MOD_ALT))
    {
        count = gui_window_count();
        for (i = 0; i < count; i++)
        {
            gui_window_t *w = gui_window_get(i);
            if (w && (w->flags & GUI_WIN_FOCUSED) && (w->flags & GUI_WIN_CLOSEABLE))
            {
                gui_window_close_animated(w);
                return;
            }
        }
        return;
    }

    /* Alt+Tab: visual window switcher */
    if (ev->type == INPUT_EVENT_TAB && (ev->modifiers & KEY_MOD_ALT))
    {
        if (!alttab_active) {
            /* build list of switchable windows */
            alttab_count = 0;
            count = gui_window_count();
            for (i = 0; i < count && alttab_count < ALTTAB_MAX_ITEMS; i++)
            {
                gui_window_t *w = gui_window_get(i);
                if (w && WIN_ON_WS(w))
                    alttab_wins[alttab_count++] = w;
            }
            if (alttab_count < 2) return;
            /* find currently focused window in list */
            alttab_sel = 0;
            for (i = 0; i < alttab_count; i++) {
                if (alttab_wins[i]->flags & GUI_WIN_FOCUSED) {
                    alttab_sel = i;
                    break;
                }
            }
            alttab_active = 1;
        }
        /* cycle to next */
        alttab_sel = (alttab_sel + 1) % alttab_count;
        gui_dirty_screen();
        return;
    }

    /* Escape dismisses alt-tab without switching */
    if (alttab_active && ev->type == INPUT_EVENT_ENTER)
    {
        /* confirm selection */
        if (alttab_sel >= 0 && alttab_sel < alttab_count) {
            gui_window_focus(alttab_wins[alttab_sel]);
        }
        alttab_active = 0;
        gui_dirty_screen();
        desktop_invalidate_taskbar();
        return;
    }

    /* Check if desktop wants this key (start menu etc) */
    if (desktop_handle_key(ev->type, ev->character))
    {
        return;
    }

    /* Deliver to focused window */
    count = gui_window_count();
    for (i = 0; i < count; i++)
    {
        gui_window_t *w = gui_window_get(i);
        if (w && (w->flags & GUI_WIN_FOCUSED))
        {
            /* Widget kit gets first crack at keyboard input */
            if (wid_handle_key(w, ev->type, ev->character))
            {
                gui_dirty_add(w->x, w->y, w->width, w->height);
                return;
            }
            if (w->on_key)
                w->on_key(w, ev->type, ev->character);
        }
    }
}

/* Mouse sensitivity: scale delta so crossing the screen takes roughly
 * the same physical mouse movement regardless of resolution.  At 640px
 * wide the multiplier is ~1×; at 1920 it's ~3×.  Non-linear: adds a
 * small acceleration for fast flicks. */
static void mouse_scale(int raw_dx, int raw_dy, int *out_dx, int *out_dy)
{
    /* base multiplier: scr_w / 640  (integer, at least 1) */
    int mul = scr_w > 640 ? scr_w / 640 : 1;
    /* small non-linear boost for speed > 4 */
    int ax = raw_dx < 0 ? -raw_dx : raw_dx;
    int ay = raw_dy < 0 ? -raw_dy : raw_dy;
    if (ax > 4) raw_dx += (raw_dx > 0 ? (ax - 4) / 2 : -(ax - 4) / 2);
    if (ay > 4) raw_dy += (raw_dy > 0 ? (ay - 4) / 2 : -(ay - 4) / 2);
    *out_dx = raw_dx * mul;
    *out_dy = raw_dy * mul;
}

static void handle_mouse(input_event_t *ev, gui_window_t **dragging_win,
                         gui_window_t **resizing_win)
{
    int old_mx = mouse_x, old_my = mouse_y;
    int dx, dy;

    mouse_scale(ev->delta_x, ev->delta_y, &dx, &dy);
    mouse_x += dx;
    mouse_y += dy;
    if (mouse_x < 0)
        mouse_x = 0;
    if (mouse_y < 0)
        mouse_y = 0;
    if (mouse_x >= scr_w)
        mouse_x = scr_w - 1;
    if (mouse_y >= scr_h)
        mouse_y = scr_h - 1;

    /* cursor moved - invalidate old and new position */
    if (mouse_x != old_mx || mouse_y != old_my)
    {
        cursor_invalidate_old();
        cursor_set_pos(mouse_x, mouse_y);
        cursor_invalidate_new();

        /* Overview mode: update hover */
        if (overview_active) {
            int old_hover = overview_hover;
            int oi;
            overview_hover = -1;
            for (oi = 0; oi < overview_count; oi++) {
                if (mouse_x >= overview_tx[oi] && mouse_x < overview_tx[oi] + overview_tw[oi] &&
                    mouse_y >= overview_ty[oi] && mouse_y < overview_ty[oi] + overview_th[oi]) {
                    overview_hover = oi;
                    break;
                }
            }
            if (overview_hover != old_hover)
                gui_dirty_screen();
        }

        /* Update dock/taskbar hover state (skip during drag) */
        if (!*dragging_win && !*resizing_win)
        {
            desktop_update_hover(mouse_x, mouse_y, ev->buttons);
            /* Update widget hover for focused window (skip when overlay open) */
            if (!desktop_is_overlay_open())
            {
                int wc = gui_window_count();
                int wi;
                for (wi = 0; wi < wc; wi++) {
                    gui_window_t *fw = gui_window_get(wi);
                    if (fw && (fw->flags & GUI_WIN_FOCUSED) &&
                        gui_window_on_current_ws(fw) &&
                        !(fw->flags & GUI_WIN_MINIMIZED)) {
                        int cx = gui_window_content_x(fw);
                        int cy = gui_window_content_y(fw);
                        wid_update_hover(fw, mouse_x - cx, mouse_y - cy);
                        break;
                    }
                }
            }
        }
    }

    /* scroll wheel — dispatch to widget under cursor in focused window */
    if (ev->scroll && !*dragging_win && !*resizing_win)
    {
        int wc = gui_window_count();
        int wi;
        for (wi = 0; wi < wc; wi++) {
            gui_window_t *fw = gui_window_get(wi);
            if (fw && (fw->flags & GUI_WIN_FOCUSED) &&
                gui_window_on_current_ws(fw) &&
                !(fw->flags & GUI_WIN_MINIMIZED)) {
                int rx = mouse_x - gui_window_content_x(fw);
                int ry = mouse_y - gui_window_content_y(fw);
                if (wid_handle_scroll(fw, rx, ry, ev->scroll)) {
                    fw->needs_redraw = 1;
                    gui_dirty_add(fw->x, fw->y, fw->width, fw->height);
                }
                break;
            }
        }
    }

    /* dragging a window — defer actual move to frame time */
    if (*dragging_win)
    {
        if (ev->buttons & 0x01)
        {
            int ow = (*dragging_win)->width, oh = (*dragging_win)->height;
            int nx = mouse_x - (*dragging_win)->drag_off_x;
            int ny = mouse_y - (*dragging_win)->drag_off_y;
            int desktop_h = scr_h - GUI_DOCK_HEIGHT;
            int menu_h = desktop_get_menubar_height();

            if (nx < -(ow - 40))
                nx = -(ow - 40);
            if (ny < menu_h)
                ny = menu_h;
            if (nx > scr_w - 40)
                nx = scr_w - 40;
            if (ny > desktop_h - GUI_TITLEBAR_HEIGHT)
                ny = desktop_h - GUI_TITLEBAR_HEIGHT;

            /* Store desired position; first pending captures the "old" pos */
            if (!drag_pending)
            {
                drag_old_x = (*dragging_win)->x;
                drag_old_y = (*dragging_win)->y;
            }
            drag_new_x = nx;
            drag_new_y = ny;
            drag_win_w = ow;
            drag_win_h = oh;
            drag_pending = 1;

            /* Detect snap zone based on mouse position */
            snap_prev_zone = snap_zone;
            if (mouse_x <= THEME_SNAP_EDGE)
                snap_zone = SNAP_LEFT;
            else if (mouse_x >= scr_w - THEME_SNAP_EDGE - 1)
                snap_zone = SNAP_RIGHT;
            else if (mouse_y <= THEME_SNAP_EDGE)
                snap_zone = SNAP_FULL;
            else
                snap_zone = SNAP_NONE;

            /* If snap zone changed, dirty the preview area */
            if (snap_zone != snap_prev_zone) {
                int desktop_h = scr_h - GUI_DOCK_HEIGHT;
                /* dirty old preview */
                if (snap_prev_zone == SNAP_LEFT)
                    gui_dirty_add(0, menu_h, scr_w / 2, desktop_h - menu_h);
                else if (snap_prev_zone == SNAP_RIGHT)
                    gui_dirty_add(scr_w / 2, menu_h, scr_w / 2, desktop_h - menu_h);
                else if (snap_prev_zone == SNAP_FULL)
                    gui_dirty_add(0, menu_h, scr_w, desktop_h - menu_h);
                /* dirty new preview */
                if (snap_zone == SNAP_LEFT)
                    gui_dirty_add(0, menu_h, scr_w / 2, desktop_h - menu_h);
                else if (snap_zone == SNAP_RIGHT)
                    gui_dirty_add(scr_w / 2, menu_h, scr_w / 2, desktop_h - menu_h);
                else if (snap_zone == SNAP_FULL)
                    gui_dirty_add(0, menu_h, scr_w, desktop_h - menu_h);
            }
        }
        else
        {
            gui_window_t *dw = *dragging_win;
            int desktop_h = scr_h - GUI_DOCK_HEIGHT;
            int menu_h2 = desktop_get_menubar_height();
            int snap_h = desktop_h - menu_h2;

            /* Apply snap if active */
            if (snap_zone != SNAP_NONE && (dw->flags & GUI_WIN_RESIZABLE))
            {
                /* Save pre-snap geometry for restore */
                if (!dw->snapped) {
                    dw->snap_restore_x = dw->x;
                    dw->snap_restore_y = dw->y;
                    dw->snap_restore_w = dw->width;
                    dw->snap_restore_h = dw->height;
                }

                if (snap_zone == SNAP_LEFT) {
                    gui_window_resize(dw, scr_w / 2, snap_h);
                    dw->x = 0;
                    dw->y = menu_h2;
                    dw->snapped = SNAP_LEFT;
                } else if (snap_zone == SNAP_RIGHT) {
                    gui_window_resize(dw, scr_w / 2, snap_h);
                    dw->x = scr_w / 2;
                    dw->y = menu_h2;
                    dw->snapped = SNAP_RIGHT;
                } else if (snap_zone == SNAP_FULL) {
                    gui_window_resize(dw, scr_w, snap_h);
                    dw->x = 0;
                    dw->y = menu_h2;
                    dw->snapped = SNAP_FULL;
                }
                gui_dirty_screen();
            }

            snap_zone = SNAP_NONE;
            snap_prev_zone = SNAP_NONE;
            dw->dragging = 0;
            *dragging_win = 0;
            drag_pending = 0;
            bg_valid = 0;
            /* Dirty full screen to ensure clean recompose after drag */
            gui_dirty_screen();
        }
        return;
    }

    /* resizing a window — apply live resize each frame */
    if (*resizing_win)
    {
        if (ev->buttons & 0x01)
        {
            gui_window_t *rw = *resizing_win;
            int dx = mouse_x - rw->resize_orig_mx;
            int dy = mouse_y - rw->resize_orig_my;
            int new_w = rw->resize_orig_w;
            int new_h = rw->resize_orig_h;

            if (rw->resizing == GUI_RESIZE_RIGHT || rw->resizing == GUI_RESIZE_BR)
                new_w += dx;
            if (rw->resizing == GUI_RESIZE_BOTTOM || rw->resizing == GUI_RESIZE_BR)
                new_h += dy;

            gui_window_resize(rw, new_w, new_h);
        }
        else
        {
            (*resizing_win)->resizing = GUI_RESIZE_NONE;
            *resizing_win = 0;
            bg_valid = 0;
        }
        return;
    }

    /* Overview mode: click to select window */
    if (overview_active && (ev->buttons & 0x01))
    {
        int oi;
        for (oi = 0; oi < overview_count; oi++) {
            if (mouse_x >= overview_tx[oi] && mouse_x < overview_tx[oi] + overview_tw[oi] &&
                mouse_y >= overview_ty[oi] && mouse_y < overview_ty[oi] + overview_th[oi]) {
                gui_window_focus(overview_wins[oi]);
                break;
            }
        }
        overview_active = 0;
        gui_dirty_screen();
        desktop_invalidate_taskbar();
        return;
    }

    if (ev->buttons & 0x01)
    {
        /* Check desktop UI clicks (taskbar, start menu) */
        if (desktop_handle_click(mouse_x, mouse_y, 1))
        {
            return;
        }

        /* If a top-level overlay (start menu) is still open after
         * desktop_handle_click, close it — the click fell on a window. */
        if (desktop_is_overlay_open()) {
            desktop_close_overlays();
            return;
        }

        gui_window_t *w = hit_test_window(mouse_x, mouse_y);
        if (w)
        {
            /* Detect drag FIRST — avoid expensive dirty-rect work that
             * would trigger a full compose cycle before the fast drag
             * path even starts. */
            if ((w->flags & GUI_WIN_DRAGGABLE) && hit_titlebar(w, mouse_x, mouse_y)
                && !hit_close_button(w, mouse_x, mouse_y)
                && !hit_minimize_button(w, mouse_x, mouse_y))
            {
                gui_window_focus(w); /* cheap: just z-order, no dirty rects */

                /* If snapped, restore original size on drag start */
                if (w->snapped) {
                    int old_w = w->width, old_h = w->height;
                    gui_dirty_add(w->x - THEME_SHADOW_EXTENT, w->y - THEME_SHADOW_EXTENT,
                                  old_w + 2 * THEME_SHADOW_EXTENT,
                                  old_h + 2 * THEME_SHADOW_EXTENT);
                    gui_window_resize(w, w->snap_restore_w, w->snap_restore_h);
                    /* reposition so cursor stays proportional on titlebar */
                    w->x = mouse_x - w->snap_restore_w / 2;
                    w->y = mouse_y - GUI_TITLEBAR_HEIGHT / 2;
                    if (w->x < 0) w->x = 0;
                    if (w->y < 0) w->y = 0;
                    w->snapped = 0;
                }

                w->dragging = 1;
                w->drag_off_x = mouse_x - w->x;
                w->drag_off_y = mouse_y - w->y;

                drag_old_x = w->x;
                drag_old_y = w->y;
                drag_new_x = w->x;
                drag_new_y = w->y;
                drag_win_w = w->width;
                drag_win_h = w->height;
                drag_pending = 0;
                snap_zone = SNAP_NONE;
                snap_prev_zone = SNAP_NONE;

                bg_valid = 0; /* snapshot will be built on first drag frame */
                *dragging_win = w;
                return;
            }

            /* Check resize edge BEFORE normal click */
            {
                int edge = hit_resize_edge(w, mouse_x, mouse_y);
                if (edge != GUI_RESIZE_NONE)
                {
                    gui_window_focus(w);
                    gui_dirty_add(w->x - THEME_SHADOW_EXTENT, w->y - THEME_SHADOW_EXTENT,
                                  w->width + 2 * THEME_SHADOW_EXTENT,
                                  w->height + 2 * THEME_SHADOW_EXTENT);

                    w->resizing = edge;
                    w->resize_orig_w = w->width;
                    w->resize_orig_h = w->height;
                    w->resize_orig_mx = mouse_x;
                    w->resize_orig_my = mouse_y;
                    *resizing_win = w;
                    return;
                }
            }

            /* Non-drag click: normal focus + dirty */
            gui_window_focus(w);
            gui_dirty_add(w->x, w->y, w->width, w->height);

            if (hit_close_button(w, mouse_x, mouse_y))
            {
                gui_window_close_animated(w);
                return;
            }

            if (hit_minimize_button(w, mouse_x, mouse_y))
            {
                w->anim_minimizing = 1;
                gui_window_anim_start(w, 0, THEME_ANIM_NORMAL);
                return;
            }

            /* new widget system handles clicks on widgets */
            {
                int rx = mouse_x - gui_window_content_x(w);
                int ry = mouse_y - gui_window_content_y(w);
                wid_handle_click(w, rx, ry, 1);
            }
            if (w->on_click)
            {
                int rx = mouse_x - gui_window_content_x(w);
                int ry = mouse_y - gui_window_content_y(w);
                w->on_click(w, rx, ry, 1);
            }

            /* Double-click detection */
            {
                unsigned int now = timer_get_uptime_ms();
                int dx = mouse_x - dblclick_last_x;
                int dy = mouse_y - dblclick_last_y;
                if (dx < 0) dx = -dx;
                if (dy < 0) dy = -dy;
                if (now - dblclick_last_ms <= DBLCLICK_MS &&
                    dx <= DBLCLICK_DIST && dy <= DBLCLICK_DIST) {
                    /* Fire double-click */
                    if (w->on_dblclick) {
                        int rx = mouse_x - gui_window_content_x(w);
                        int ry = mouse_y - gui_window_content_y(w);
                        w->on_dblclick(w, rx, ry);
                    }
                    dblclick_last_ms = 0; /* reset to avoid triple-click */
                } else {
                    dblclick_last_ms = now;
                    dblclick_last_x = mouse_x;
                    dblclick_last_y = mouse_y;
                }
            }
        }
    }

    /* right-click handling */
    if (ev->buttons & 0x02)
    {
        if (desktop_handle_click(mouse_x, mouse_y, 2))
        {
            return;
        }
    }
}

/* ==================================================================
 *  Public API
 * ================================================================== */

void gui_get_metrics(gui_metrics_t *out)
{
    if (out)
        *out = metrics;
}

void gui_init(void)
{
    scr_w = (int)fb_width();
    scr_h = (int)fb_height();
    scr_pitch = fb_pitch();

    /* initialise renderer: try virtio-gpu, falls back to SW internally */
    if (gpu_init_virtio(scr_w, scr_h, (int)fb_bpp()) != 0)
        return;

    /* keep backbuffer / bb_surf aliases for existing compositor code */
    backbuffer = g_gpu.backbuffer->pixels;
    backbuffer_phys = g_gpu.backbuffer->alloc_phys;
    backbuffer_size = g_gpu.backbuffer->alloc_size;
    bb_surf.pixels     = backbuffer;
    bb_surf.width      = scr_w;
    bb_surf.height     = scr_h;
    bb_surf.stride     = scr_w;
    bb_surf.alloc_phys = 0;
    bb_surf.alloc_size = 0;

    /* allocate background cache for smooth window dragging */
    bg_buffer_phys = physmem_alloc_region(backbuffer_size, 4096);
    if (bg_buffer_phys)
    {
        bg_buffer = (uint32_t *)(uintptr_t)bg_buffer_phys;
        memset(bg_buffer, 0, backbuffer_size);
    }
    bg_valid = 0;

    mouse_x = scr_w / 2;
    mouse_y = scr_h / 2;
    gui_running = 0;
    need_compose = 1;

    memset(&metrics, 0, sizeof(metrics));

    fb_hide_mouse_cursor();
    theme_init();
    cursor_init(scr_w, scr_h);
    cursor_set_pos(mouse_x, mouse_y);
    desktop_init(scr_w, scr_h);
    theme_load();  /* restore saved theme/accent/autohide from /etc/gui.conf */

    /* mark entire screen dirty for first paint */
    gui_dirty_screen();
}

void gui_stop(void)
{
    gui_running = 0;
}

void gui_request_redraw(void)
{
    need_compose = 1;
}

int gui_is_active(void)
{
    return gui_running;
}

int gui_screen_width(void)
{
    return scr_w;
}

int gui_screen_height(void)
{
    return scr_h;
}

gui_surface_t *gui_get_backbuffer(void)
{
    return &bb_surf;
}

int gui_resize_screen(int new_w, int new_h)
{
    if (new_w <= 0 || new_h <= 0) return -1;
    if (new_w == scr_w && new_h == scr_h) return 0;

    /* 1. Ask the renderer to reallocate the backbuffer */
    if (!g_gpu.ops->resize || g_gpu.ops->resize(new_w, new_h) != 0)
        return -1;

    /* 2. Free the old bg_buffer */
    if (bg_buffer_phys)
        physmem_free_region(bg_buffer_phys, backbuffer_size);

    /* 3. Update compositor screen dimensions */
    scr_w = new_w;
    scr_h = new_h;
    scr_pitch = (uint32_t)new_w * 4;

    /* 4. Refresh backbuffer aliases */
    backbuffer      = g_gpu.backbuffer->pixels;
    backbuffer_phys = g_gpu.backbuffer->alloc_phys;
    backbuffer_size = g_gpu.backbuffer->alloc_size;
    bb_surf.pixels  = backbuffer;
    bb_surf.width   = scr_w;
    bb_surf.height  = scr_h;
    bb_surf.stride  = scr_w;

    /* 5. Reallocate bg_buffer for drag cache */
    bg_buffer_phys = physmem_alloc_region(backbuffer_size, 4096);
    if (bg_buffer_phys) {
        bg_buffer = (uint32_t *)(uintptr_t)bg_buffer_phys;
        memset(bg_buffer, 0, backbuffer_size);
    } else {
        bg_buffer = 0;
    }
    bg_valid = 0;

    /* 6. Resize subsystems */
    cursor_resize(new_w, new_h);
    desktop_resize(new_w, new_h);

    /* 7. Clamp mouse position */
    if (mouse_x >= scr_w) mouse_x = scr_w - 1;
    if (mouse_y >= scr_h) mouse_y = scr_h - 1;
    cursor_set_pos(mouse_x, mouse_y);

    /* 8. Clamp windows that would fall off-screen */
    {
        int i, count = gui_window_count();
        for (i = 0; i < count; i++) {
            gui_window_t *win = gui_window_get(i);
            if (!win) continue;
            if (win->x + win->width > scr_w)
                win->x = scr_w - win->width;
            if (win->y + win->height > scr_h)
                win->y = scr_h - win->height;
            if (win->x < 0) win->x = 0;
            if (win->y < 0) win->y = 0;
            win->needs_redraw = 1;
        }
    }

    /* 9. Full repaint */
    gui_dirty_screen();
    need_compose = 1;
    return 0;
}

void gui_run(void)
{
    input_event_t ev;
    gui_window_t *dragging_win = 0;
    gui_window_t *resizing_win = 0;
    unsigned int last_frame_ms;
    unsigned int last_tick_sec = 0;

    gui_running = 1;
    need_compose = 1;
    last_frame_ms = timer_get_uptime_ms();
    fps_timer_ms = last_frame_ms;
    present_count = 0;
    frame_time_acc = 0;
    frame_time_samples = 0;
    frame_time_max_this_sec = 0;

    while (gui_running)
    {
        int mouse_dx = 0, mouse_dy = 0;
        int mouse_btn = 0, had_mouse = 0;
        int mouse_scroll = 0;
        unsigned int coalesced = 0;

        loop_total_count++;

        usb_hid_poll();
        e1000_poll_rx();

        /* drain all pending input, coalescing mouse moves */
        while (input_poll_event(&ev))
        {
            if (ev.device_type == INPUT_DEVICE_KEYBOARD)
            {
                handle_keyboard(&ev);
                if (!gui_running)
                    break;
            }
            if (ev.device_type == INPUT_DEVICE_MOUSE)
            {
                mouse_dx += ev.delta_x;
                mouse_dy += ev.delta_y;
                mouse_scroll += ev.scroll;
                mouse_btn = ev.buttons;
                had_mouse = 1;
                coalesced++;
            }
        }
        if (!gui_running)
            break;

        /* process coalesced mouse as a single event */
        if (had_mouse)
        {
            input_event_t coalesced_ev;
            coalesced_ev.device_type = INPUT_DEVICE_MOUSE;
            coalesced_ev.delta_x = mouse_dx;
            coalesced_ev.delta_y = mouse_dy;
            coalesced_ev.scroll = mouse_scroll;
            coalesced_ev.buttons = mouse_btn;
            coalesced_ev.type = 0;
            coalesced_ev.character = 0;
            handle_mouse(&coalesced_ev, &dragging_win, &resizing_win);
            metrics.coalesced_moves = coalesced > 1 ? coalesced - 1 : 0;
        }

        /* detect Alt release to confirm alt-tab selection */
        if (alttab_active && !(keyboard_get_modifiers() & KEY_MOD_ALT))
        {
            if (alttab_sel >= 0 && alttab_sel < alttab_count)
                gui_window_focus(alttab_wins[alttab_sel]);
            alttab_active = 0;
            gui_dirty_screen();
            desktop_invalidate_taskbar();
        }

        /* track drag state in metrics */
        metrics.drag_active = (dragging_win != 0 || resizing_win != 0) ? 1 : 0;
        metrics.drag_mouse_x = (unsigned int)mouse_x;
        metrics.drag_mouse_y = (unsigned int)mouse_y;
        if (dragging_win)
        {
            metrics.drag_pending_count++;
        }

        /* periodic: update clock every second (skip during drag to avoid
         * expensive desktop rebuild interrupting the fast path) */
        if (!dragging_win)
        {
            unsigned int now_sec = timer_get_uptime_ms() / 1000;
            if (now_sec != last_tick_sec)
            {
                last_tick_sec = now_sec;
                desktop_on_tick();
                notify_tick();
                osd_tick();
            }
            /* Animation ticks run every frame, not just once/sec */
            desktop_anim_tick();
            gui_window_anim_tick();
        }

        /* ====================== FAST DRAG PATH (VBlank-synced) ====================== */
        if (dragging_win && bg_buffer)
        {
            if (drag_pending)
            {
                unsigned int drag_t0, drag_t1, drag_ft;
                int old_x, old_y, new_x, new_y;
                int ux, uy, uw, uh;
                int sx, sy, bw, bh, row;

                drag_t0 = timer_get_monotonic_us();

                if (!bg_valid)
                    rebuild_bg(dragging_win);

                /* Capture positions (IRQs may update drag_new during VBlank wait) */
                old_x = drag_old_x;
                old_y = drag_old_y;
                new_x = drag_new_x;
                new_y = drag_new_y;

                dragging_win->x = new_x;
                dragging_win->y = new_y;

                /* During drag use a single light shadow for speed;
                 * extent = OFF0 + SPREAD0 (typically 4 instead of 14). */
                #define DRAG_SHADOW_EXT (THEME_SHADOW_OFF0 + THEME_SHADOW_SPREAD0)

                /* Compute union rect (old + new), clipped to screen */
                ux = (old_x < new_x ? old_x : new_x) - DRAG_SHADOW_EXT;
                uy = (old_y < new_y ? old_y : new_y) - DRAG_SHADOW_EXT;
                uw = (old_x > new_x ? old_x : new_x) + drag_win_w + DRAG_SHADOW_EXT - ux;
                uh = (old_y > new_y ? old_y : new_y) + drag_win_h + DRAG_SHADOW_EXT - uy;
                if (ux < 0) { uw += ux; ux = 0; }
                if (uy < 0) { uh += uy; uy = 0; }
                if (ux + uw > scr_w) uw = scr_w - ux;
                if (uy + uh > scr_h) uh = scr_h - uy;

                /* 1. Restore background in dirty zone */
                for (row = uy; row < uy + uh; row++)
                    memcpy(&backbuffer[row * scr_w + ux],
                           &bg_buffer[row * scr_w + ux],
                           (size_t)uw * 4);

                /* 2a. Draw single light shadow at new position (fast) */
                gpu_set_target(g_gpu.backbuffer);
                {
                    int da = THEME_SHADOW_ALPHA0 * theme.shadow_alpha_mul / 100;
                    if (da > 255) da = 255;
                    gpu_shadow(new_x, new_y, drag_win_w, drag_win_h,
                               THEME_SHADOW_OFF0, THEME_SHADOW_SPREAD0, da);
                }

                /* 2b. Blit window at new position */
                sx = new_x < 0 ? -new_x : 0;
                sy = new_y < 0 ? -new_y : 0;
                bw = drag_win_w - sx;
                bh = drag_win_h - sy;
                if (new_x + sx + bw > scr_w) bw = scr_w - (new_x + sx);
                if (new_y + sy + bh > scr_h) bh = scr_h - (new_y + sy);
                if (bw > 0 && bh > 0) {
                    gpu_texture_t drag_tex;
                    gpu_texture_wrap(&drag_tex, dragging_win->surface.pixels,
                                     dragging_win->surface.width,
                                     dragging_win->surface.height,
                                     dragging_win->surface.stride);
                    gpu_set_target(g_gpu.backbuffer);
                    gpu_blit(new_x + sx, new_y + sy, &drag_tex, sx, sy, bw, bh);
                }

                /* 2c. Draw snap preview overlay if active */
                if (snap_zone != SNAP_NONE) {
                    int sx0, sy0, sw0, sh0;
                    int desktop_h = scr_h - GUI_DOCK_HEIGHT;
                    int snap_top = desktop_get_menubar_height();
                    int snap_area_h = desktop_h - snap_top;
                    uint32_t scol = THEME_SNAP_PREVIEW_COL;
                    int salpha = THEME_SNAP_PREVIEW_ALPHA;

                    if (snap_zone == SNAP_LEFT)       { sx0 = 4; sy0 = snap_top + 4; sw0 = scr_w/2 - 8;  sh0 = snap_area_h - 8; }
                    else if (snap_zone == SNAP_RIGHT)  { sx0 = scr_w/2 + 4; sy0 = snap_top + 4; sw0 = scr_w/2 - 8;  sh0 = snap_area_h - 8; }
                    else /* SNAP_FULL */               { sx0 = 4; sy0 = snap_top + 4; sw0 = scr_w - 8; sh0 = snap_area_h - 8; }

                    gpu_set_target(g_gpu.backbuffer);
                    /* top border */
                    gpu_fill_rect_alpha(sx0, sy0, sw0, 3, scol, salpha);
                    /* bottom border */
                    gpu_fill_rect_alpha(sx0, sy0 + sh0 - 3, sw0, 3, scol, salpha);
                    /* left border */
                    gpu_fill_rect_alpha(sx0, sy0 + 3, 3, sh0 - 6, scol, salpha);
                    /* right border */
                    gpu_fill_rect_alpha(sx0 + sw0 - 3, sy0 + 3, 3, sh0 - 6, scol, salpha);
                }

                /* 3. Draw cursor on top */
                cursor_draw(&bb_surf);

                /* 4. Wait for VBlank, then present to real framebuffer */
                gpu_vsync_wait();
                gpu_present(ux, uy, uw, uh);

                /* 5. Clean cursor from backbuffer */
                cursor_erase(&bb_surf);

                drag_old_x = new_x;
                drag_old_y = new_y;
                drag_pending = 0;

                /* metrics */
                drag_t1 = timer_get_monotonic_us();
                drag_ft = drag_t1 - drag_t0;
                metrics.frame_time_us = drag_ft;
                frame_time_acc += drag_ft;
                frame_time_samples++;
                if (drag_ft > frame_time_max_this_sec)
                    frame_time_max_this_sec = drag_ft;
                metrics.drag_move_count++;
                metrics.drag_win_x = (unsigned int)new_x;
                metrics.drag_win_y = (unsigned int)new_y;

                last_frame_ms = timer_get_uptime_ms();
                present_count++;
                loop_busy_count++;
            }

            dirty_count = 0;
            need_compose = 0;
        }
        else if (need_compose || dirty_count > 0)
        {
            /* ---- NORMAL PATH: dirty-rect compositing with 60 fps cap ---- */
            unsigned int now_ms = timer_get_uptime_ms();
            unsigned int elapsed = now_ms - last_frame_ms;

            /* invalidate bg cache since non-drag state changed */
            bg_valid = 0;

            if (elapsed >= FRAME_INTERVAL_MS)
            {
                unsigned int frame_start, frame_end, ft;

                frame_start = timer_get_monotonic_us();

                /* erase cursor from backbuffer (restore underlying pixels) */
                cursor_erase(&bb_surf);

                /* compose dirty regions */
                compose_frame();

                /* draw cursor on top */
                cursor_draw(&bb_surf);

                /* dirty rect debug overlay: flash colored borders */
                if (dirty_debug) {
                    int di;
                    gpu_set_target(g_gpu.backbuffer);
                    for (di = 0; di < dirty_count; di++) {
                        gui_dirty_rect_t *dr = &dirty_list[di];
                        /* Cycle through distinct colours per rect */
                        uint32_t dcols[] = {0xFF0000, 0x00FF00, 0x0000FF,
                                            0xFFFF00, 0xFF00FF, 0x00FFFF};
                        uint32_t dc = dcols[di % 6];
                        int da = 180;
                        /* top */
                        gpu_fill_rect_alpha(dr->x, dr->y, dr->w, 2, dc, da);
                        /* bottom */
                        gpu_fill_rect_alpha(dr->x, dr->y + dr->h - 2, dr->w, 2, dc, da);
                        /* left */
                        gpu_fill_rect_alpha(dr->x, dr->y, 2, dr->h, dc, da);
                        /* right */
                        gpu_fill_rect_alpha(dr->x + dr->w - 2, dr->y, 2, dr->h, dc, da);
                    }
                }

                /* present dirty regions to framebuffer */
                present_dirty();

                frame_end = timer_get_monotonic_us();
                ft = frame_end - frame_start;
                metrics.frame_time_us = ft;

                /* record into frame time history ring buffer */
                ft_hist[ft_hist_idx] = ft;
                ft_hist_idx = (ft_hist_idx + 1) % FT_HIST_LEN;

                /* count dropped frames (exceeding 60fps budget) */
                if (ft > 16667) dropped_frames++;

                /* accumulate for rolling average and max */
                frame_time_acc += ft;
                frame_time_samples++;
                if (ft > frame_time_max_this_sec)
                    frame_time_max_this_sec = ft;

                /* clear dirty list */
                dirty_count = 0;
                need_compose = 0;
                last_frame_ms = now_ms;

                present_count++;
                loop_busy_count++;
            }
        }

        /* ---- FPS / metrics update (works for both drag and normal paths) ---- */
        {
            unsigned int now_ms = timer_get_uptime_ms();
            if (now_ms - fps_timer_ms >= 1000)
            {
                metrics.fps = present_count;
                present_count = 0;
                fps_timer_ms = now_ms;
                if (frame_time_samples > 0)
                {
                    metrics.frame_time_avg = frame_time_acc / frame_time_samples;
                }
                metrics.frame_time_max = frame_time_max_this_sec;
                frame_time_acc = 0;
                frame_time_samples = 0;
                frame_time_max_this_sec = 0;
                metrics.drag_move_count = 0;
                metrics.drag_pending_count = 0;

                /* CPU idle: % of loop iters that did NOT compose */
                if (loop_total_count > 0)
                    metrics.idle_pct = ((loop_total_count - loop_busy_count) * 100) / loop_total_count;
                else
                    metrics.idle_pct = 100;
                loop_total_count = 0;
                loop_busy_count = 0;

                /* VRAM: backbuffer + bg_buffer + all window surfaces */
                {
                    unsigned int vram = 0;
                    int vi, vc;
                    /* backbuffer: scr_w * scr_h * 4 bytes */
                    vram += (unsigned int)(scr_w * scr_h) * 4;
                    /* bg_buffer: same size */
                    if (bg_buffer)
                        vram += (unsigned int)(scr_w * scr_h) * 4;
                    /* window surfaces */
                    vc = gui_window_count();
                    for (vi = 0; vi < vc; vi++) {
                        gui_window_t *vw = gui_window_get(vi);
                        if (vw && vw->surface.pixels)
                            vram += (unsigned int)(vw->surface.width * vw->surface.height) * 4;
                    }
                    metrics.vram_kb = vram / 1024;
                }

                /* refresh perf overlay each second */
                if (perf_overlay)
                    gui_dirty_add(PERF_OVL_X - 4, PERF_OVL_Y - 4,
                                  PERF_OVL_W, PERF_OVL_TOTAL_H);
            }
        }

        /* Sleep: hlt until next IRQ (timer, mouse, keyboard).
         * VBlank sync in the drag path handles frame pacing. */
        __asm__ volatile("hlt");
    }

    /* cleanup */
    desktop_shutdown();

    {
        int i, count = gui_window_count();
        for (i = count - 1; i >= 0; i--)
        {
            gui_window_t *w = gui_window_get(i);
            if (w)
                gui_window_destroy(w);
        }
    }

    if (backbuffer)
    {
        physmem_free_region(backbuffer_phys, backbuffer_size);
        backbuffer = 0;
        backbuffer_phys = 0;
    }

    if (bg_buffer)
    {
        physmem_free_region(bg_buffer_phys, backbuffer_size);
        bg_buffer = 0;
        bg_buffer_phys = 0;
    }

    fb_clear();
}
