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

/* ---- state ---- */
static int gui_running;
static int mouse_x, mouse_y;
static int need_compose;

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

    /* 2. Draw window shadows, then blit windows on top */
    for (i = 0; i < wcount; i++)
    {
        gui_window_t *w = sorted[i];
        int wx0, wy0, wx1, wy1;
        int sx, sy, dstx, dsty, bw, bh;

        if (!(w->flags & GUI_WIN_VISIBLE) || (w->flags & GUI_WIN_MINIMIZED))
            continue;
        if (!w->surface.pixels)
            continue;

        /* Draw multi-layer soft drop shadow (3 concentric rings) */
        {
            static const int sh_off[]    = { THEME_SHADOW_OFF0,    THEME_SHADOW_OFF1,    THEME_SHADOW_OFF2 };
            static const int sh_spread[] = { THEME_SHADOW_SPREAD0, THEME_SHADOW_SPREAD1, THEME_SHADOW_SPREAD2 };
            static const int sh_alpha[]  = { THEME_SHADOW_ALPHA0,  THEME_SHADOW_ALPHA1,  THEME_SHADOW_ALPHA2 };
            int layer;
            for (layer = THEME_SHADOW_LAYERS - 1; layer >= 0; layer--) {
                int off = sh_off[layer], spread = sh_spread[layer], alpha = sh_alpha[layer];
                int ia = 255 - alpha;
                int shx = w->x + off, shy = w->y + off;
                int shw = w->width + spread, shh = w->height + spread;
                int s0x = shx > dx0 ? shx : dx0;
                int s0y = shy > dy0 ? shy : dy0;
                int s1x = (shx + shw) < dx1 ? (shx + shw) : dx1;
                int s1y = (shy + shh) < dy1 ? (shy + shh) : dy1;
                if (s0x >= s1x || s0y >= s1y) continue;
                for (row = s0y; row < s1y; row++) {
                    uint32_t *p = &backbuffer[row * scr_w + s0x];
                    int cx;
                    for (cx = 0; cx < s1x - s0x; cx++) {
                        int px = s0x + cx, py = row;
                        if (px >= w->x && px < w->x + w->width &&
                            py >= w->y && py < w->y + w->height)
                            continue;
                        uint32_t bg = p[cx];
                        uint32_t r = ((bg >> 16) & 0xFF) * (uint32_t)ia / 255;
                        uint32_t g = ((bg >> 8) & 0xFF) * (uint32_t)ia / 255;
                        uint32_t b = (bg & 0xFF) * (uint32_t)ia / 255;
                        p[cx] = (r << 16) | (g << 8) | b;
                    }
                }
            }
        }
    }

    /* 3. Blit each window that overlaps this dirty rect */
    for (i = 0; i < wcount; i++)
    {
        gui_window_t *w = sorted[i];
        int wx0, wy0, wx1, wy1;
        int sx, sy, dstx, dsty, bw, bh;

        if (!(w->flags & GUI_WIN_VISIBLE) || (w->flags & GUI_WIN_MINIMIZED))
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

        /* blit from window surface to backbuffer (with rounded corner mask) */
        {
            int cr = (w->flags & GUI_WIN_NO_DECOR) ? 0 : THEME_WIN_RADIUS;
            for (row = 0; row < bh; row++)
            {
                int src_row = sy + row;
                int dst_row = dsty + row;
                int left_inset = 0, right_inset = 0;
                if (src_row < 0 || src_row >= w->surface.height)
                    continue;
                if (dst_row < 0 || dst_row >= scr_h)
                    continue;

                /* compute per-row corner insets */
                if (cr > 0) {
                    /* top corners */
                    if (src_row == 0)      { left_inset = 3; right_inset = 3; }
                    else if (src_row == 1) { left_inset = 2; right_inset = 2; }
                    else if (src_row == 2) { left_inset = 1; right_inset = 1; }
                    /* bottom corners */
                    if (src_row == w->surface.height - 1)      { left_inset = 3; right_inset = 3; }
                    else if (src_row == w->surface.height - 2) { left_inset = 2; right_inset = 2; }
                    else if (src_row == w->surface.height - 3) { left_inset = 1; right_inset = 1; }
                }

                if (left_inset == 0 && right_inset == 0) {
                    memcpy(&backbuffer[dst_row * scr_w + dstx],
                           &w->surface.pixels[src_row * w->surface.stride + sx],
                           (size_t)bw * 4);
                } else {
                    /* adjust for the case the blit region doesn't start at column 0 */
                    int row_sx = sx;          /* first source column in this blit */
                    int row_bw = bw;
                    int row_dstx = dstx;

                    /* left inset: skip pixels at start of window row */
                    if (row_sx < left_inset) {
                        int skip = left_inset - row_sx;
                        if (skip > row_bw) skip = row_bw;
                        row_sx += skip;
                        row_dstx += skip;
                        row_bw -= skip;
                    }
                    /* right inset: skip pixels at end of window row */
                    if (row_sx + row_bw > w->width - right_inset) {
                        int over = (row_sx + row_bw) - (w->width - right_inset);
                        if (over > row_bw) over = row_bw;
                        row_bw -= over;
                    }
                    if (row_bw > 0) {
                        memcpy(&backbuffer[dst_row * scr_w + row_dstx],
                               &w->surface.pixels[src_row * w->surface.stride + row_sx],
                               (size_t)row_bw * 4);
                    }
                }
            }
        }
    }

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

        wcount = gui_window_count();
        for (i = 0; i < wcount; i++)
        {
            gui_window_t *w = gui_window_get(i);
            if (w && w->needs_redraw && (w->flags & GUI_WIN_VISIBLE))
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
                    w->needs_redraw = 0;
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
        if (!(w->flags & GUI_WIN_VISIBLE) || (w->flags & GUI_WIN_MINIMIZED))
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
    uint8_t *fb;
    int i;
    unsigned int t0, t1;

    fb = (uint8_t *)fb_get_buffer();
    if (!fb)
        return;

    t0 = timer_get_monotonic_us();

    if (fb_bpp() == 32)
    {
        /* fast path: direct memcpy per dirty rect scanlines */
        for (i = 0; i < dirty_count; i++)
        {
            gui_dirty_rect_t *dr = &dirty_list[i];
            int y;
            for (y = dr->y; y < dr->y + dr->h && y < scr_h; y++)
            {
                memcpy(fb + y * scr_pitch + dr->x * 4,
                       &backbuffer[y * scr_w + dr->x],
                       (size_t)dr->w * 4);
            }
        }
    }
    else
    {
        /* slow path for non-32bpp: full frame present */
        fb_present_rgb32(backbuffer, (uint32_t)scr_w, (uint32_t)scr_h, (uint32_t)scr_w);
    }

    t1 = timer_get_monotonic_us();
    metrics.present_us = t1 - t0;
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
        if (!w || !(w->flags & GUI_WIN_VISIBLE) || (w->flags & GUI_WIN_MINIMIZED))
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

static gui_widget_t *hit_widget(gui_window_t *win, int mx, int my)
{
    int cx = gui_window_content_x(win);
    int cy = gui_window_content_y(win);
    int i;
    for (i = 0; i < win->widget_count; i++)
    {
        gui_widget_t *w = &win->widgets[i];
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

    /* Alt+F4: close the focused window */
    if (ev->type == INPUT_EVENT_F4 && (ev->modifiers & KEY_MOD_ALT))
    {
        count = gui_window_count();
        for (i = 0; i < count; i++)
        {
            gui_window_t *w = gui_window_get(i);
            if (w && (w->flags & GUI_WIN_FOCUSED) && (w->flags & GUI_WIN_CLOSEABLE))
            {
                gui_dirty_add(w->x - 6, w->y - 6, w->width + 12, w->height + 12);
                if (w->on_close)
                    w->on_close(w);
                else
                    gui_window_destroy(w);
                return;
            }
        }
        return;
    }

    /* Alt+Tab: cycle focus to next visible window */
    if (ev->type == INPUT_EVENT_TAB && (ev->modifiers & KEY_MOD_ALT))
    {
        count = gui_window_count();
        if (count < 2) return;
        int focused_idx = -1;
        for (i = 0; i < count; i++)
        {
            gui_window_t *w = gui_window_get(i);
            if (w && (w->flags & GUI_WIN_FOCUSED))
            {
                focused_idx = i;
                break;
            }
        }
        /* Find next visible window after the focused one */
        for (i = 1; i < count; i++)
        {
            int idx = (focused_idx + i) % count;
            gui_window_t *w = gui_window_get(idx);
            if (w && (w->flags & GUI_WIN_VISIBLE) &&
                !(w->flags & GUI_WIN_MINIMIZED))
            {
                gui_window_focus(w);
                gui_dirty_add(w->x, w->y, w->width, w->height);
                /* Also dirty the previously focused window */
                if (focused_idx >= 0)
                {
                    gui_window_t *prev = gui_window_get(focused_idx);
                    if (prev)
                        gui_dirty_add(prev->x, prev->y, prev->width, prev->height);
                }
                desktop_invalidate_taskbar();
                return;
            }
        }
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
        if (w && (w->flags & GUI_WIN_FOCUSED) && w->on_key)
        {
            w->on_key(w, ev->type, ev->character);
        }
    }
}

static void handle_mouse(input_event_t *ev, gui_window_t **dragging_win,
                         gui_window_t **resizing_win)
{
    int old_mx = mouse_x, old_my = mouse_y;

    mouse_x += ev->delta_x;
    mouse_y += ev->delta_y;
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

        /* Update dock/taskbar hover state (skip during drag) */
        if (!*dragging_win && !*resizing_win)
            desktop_update_hover(mouse_x, mouse_y);
    }

    /* dragging a window — defer actual move to frame time */
    if (*dragging_win)
    {
        if (ev->buttons & 0x01)
        {
            int ow = (*dragging_win)->width, oh = (*dragging_win)->height;
            int nx = mouse_x - (*dragging_win)->drag_off_x;
            int ny = mouse_y - (*dragging_win)->drag_off_y;
            int desktop_h = scr_h - GUI_TASKBAR_HEIGHT;
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
                int desktop_h = scr_h - GUI_TASKBAR_HEIGHT;
                /* dirty old preview */
                if (snap_prev_zone == SNAP_LEFT)
                    gui_dirty_add(0, 0, scr_w / 2, desktop_h);
                else if (snap_prev_zone == SNAP_RIGHT)
                    gui_dirty_add(scr_w / 2, 0, scr_w / 2, desktop_h);
                else if (snap_prev_zone == SNAP_FULL)
                    gui_dirty_add(0, 0, scr_w, desktop_h);
                /* dirty new preview */
                if (snap_zone == SNAP_LEFT)
                    gui_dirty_add(0, 0, scr_w / 2, desktop_h);
                else if (snap_zone == SNAP_RIGHT)
                    gui_dirty_add(scr_w / 2, 0, scr_w / 2, desktop_h);
                else if (snap_zone == SNAP_FULL)
                    gui_dirty_add(0, 0, scr_w, desktop_h);
            }
        }
        else
        {
            gui_window_t *dw = *dragging_win;
            int desktop_h = scr_h - GUI_TASKBAR_HEIGHT;

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
                    gui_window_resize(dw, scr_w / 2, desktop_h);
                    dw->x = 0;
                    dw->y = 0;
                    dw->snapped = SNAP_LEFT;
                } else if (snap_zone == SNAP_RIGHT) {
                    gui_window_resize(dw, scr_w / 2, desktop_h);
                    dw->x = scr_w / 2;
                    dw->y = 0;
                    dw->snapped = SNAP_RIGHT;
                } else if (snap_zone == SNAP_FULL) {
                    gui_window_resize(dw, scr_w, desktop_h);
                    dw->x = 0;
                    dw->y = 0;
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
                    gui_dirty_add(w->x, w->y,
                                  old_w + THEME_SHADOW_EXTENT,
                                  old_h + THEME_SHADOW_EXTENT);
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
                    gui_dirty_add(w->x, w->y, w->width + THEME_SHADOW_EXTENT,
                                  w->height + THEME_SHADOW_EXTENT);

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
                gui_dirty_add(w->x - 6, w->y - 6, w->width + 12, w->height + 12);
                if (w->on_close)
                    w->on_close(w);
                else
                    gui_window_destroy(w);
                return;
            }

            if (hit_minimize_button(w, mouse_x, mouse_y))
            {
                w->flags |= GUI_WIN_MINIMIZED;
                gui_dirty_add(w->x, w->y,
                              w->width + THEME_SHADOW_EXTENT,
                              w->height + THEME_SHADOW_EXTENT);
                desktop_invalidate_taskbar();
                return;
            }

            gui_widget_t *wi = hit_widget(w, mouse_x, mouse_y);
            if (wi && wi->on_click)
            {
                wi->on_click(wi);
            }
            if (w->on_click)
            {
                int rx = mouse_x - gui_window_content_x(w);
                int ry = mouse_y - gui_window_content_y(w);
                w->on_click(w, rx, ry, 1);
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

    /* allocate backbuffer */
    backbuffer_size = (uint32_t)(scr_w * scr_h * 4);
    backbuffer_phys = physmem_alloc_region(backbuffer_size, 4096);
    if (!backbuffer_phys)
        return;
    backbuffer = (uint32_t *)(uintptr_t)backbuffer_phys;
    memset(backbuffer, 0, backbuffer_size);

    /* set up backbuffer as surface for compositing into */
    bb_surf.pixels = backbuffer;
    bb_surf.width = scr_w;
    bb_surf.height = scr_h;
    bb_surf.stride = scr_w;
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
    cursor_init(scr_w, scr_h);
    cursor_set_pos(mouse_x, mouse_y);
    desktop_init(scr_w, scr_h);

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
        unsigned int coalesced = 0;

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
            coalesced_ev.buttons = mouse_btn;
            coalesced_ev.type = 0;
            coalesced_ev.character = 0;
            handle_mouse(&coalesced_ev, &dragging_win, &resizing_win);
            metrics.coalesced_moves = coalesced > 1 ? coalesced - 1 : 0;
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
            }
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
                uint8_t *fb;

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

                /* Compute union rect (old + new), clipped to screen */
                ux = old_x < new_x ? old_x : new_x;
                uy = old_y < new_y ? old_y : new_y;
                uw = (old_x > new_x ? old_x : new_x) + drag_win_w + THEME_SHADOW_EXTENT - ux;
                uh = (old_y > new_y ? old_y : new_y) + drag_win_h + THEME_SHADOW_EXTENT - uy;
                if (ux < 0) { uw += ux; ux = 0; }
                if (uy < 0) { uh += uy; uy = 0; }
                if (ux + uw > scr_w) uw = scr_w - ux;
                if (uy + uh > scr_h) uh = scr_h - uy;

                /* 1. Restore background in dirty zone */
                for (row = uy; row < uy + uh; row++)
                    memcpy(&backbuffer[row * scr_w + ux],
                           &bg_buffer[row * scr_w + ux],
                           (size_t)uw * 4);

                /* 2a. Draw shadow at new position */
                {
                    static const int sh_off[]    = { THEME_SHADOW_OFF0,    THEME_SHADOW_OFF1,    THEME_SHADOW_OFF2 };
                    static const int sh_spread[] = { THEME_SHADOW_SPREAD0, THEME_SHADOW_SPREAD1, THEME_SHADOW_SPREAD2 };
                    static const int sh_alpha[]  = { THEME_SHADOW_ALPHA0,  THEME_SHADOW_ALPHA1,  THEME_SHADOW_ALPHA2 };
                    int layer;
                    for (layer = THEME_SHADOW_LAYERS - 1; layer >= 0; layer--) {
                        int off = sh_off[layer], spread = sh_spread[layer], alpha = sh_alpha[layer];
                        int ia = 255 - alpha;
                        int shx = new_x + off, shy = new_y + off;
                        int shw = drag_win_w + spread, shh = drag_win_h + spread;
                        int s0x = shx < 0 ? 0 : shx;
                        int s0y = shy < 0 ? 0 : shy;
                        int s1x = shx + shw > scr_w ? scr_w : shx + shw;
                        int s1y = shy + shh > scr_h ? scr_h : shy + shh;
                        int sr;
                        if (s0x >= s1x || s0y >= s1y) continue;
                        for (sr = s0y; sr < s1y; sr++) {
                            uint32_t *p = &backbuffer[sr * scr_w + s0x];
                            int cx;
                            for (cx = 0; cx < s1x - s0x; cx++) {
                                int px = s0x + cx, py = sr;
                                if (px >= new_x && px < new_x + drag_win_w &&
                                    py >= new_y && py < new_y + drag_win_h)
                                    continue;
                                uint32_t bg = p[cx];
                                uint32_t rr = ((bg >> 16) & 0xFF) * (uint32_t)ia / 255;
                                uint32_t gg = ((bg >> 8) & 0xFF) * (uint32_t)ia / 255;
                                uint32_t bb = (bg & 0xFF) * (uint32_t)ia / 255;
                                p[cx] = (rr << 16) | (gg << 8) | bb;
                            }
                        }
                    }
                }

                /* 2b. Blit window at new position */
                sx = new_x < 0 ? -new_x : 0;
                sy = new_y < 0 ? -new_y : 0;
                bw = drag_win_w - sx;
                bh = drag_win_h - sy;
                if (new_x + sx + bw > scr_w) bw = scr_w - (new_x + sx);
                if (new_y + sy + bh > scr_h) bh = scr_h - (new_y + sy);
                if (bw > 0 && bh > 0) {
                    for (row = 0; row < bh; row++)
                        memcpy(&backbuffer[(new_y + sy + row) * scr_w + (new_x + sx)],
                               &dragging_win->surface.pixels[(sy + row) * dragging_win->surface.stride + sx],
                               (size_t)bw * 4);
                }

                /* 2c. Draw snap preview overlay if active */
                if (snap_zone != SNAP_NONE) {
                    int sx0, sy0, sw0, sh0;
                    int desktop_h = scr_h - GUI_TASKBAR_HEIGHT;
                    uint32_t scol = THEME_SNAP_PREVIEW_COL;
                    int salpha = THEME_SNAP_PREVIEW_ALPHA;
                    int sia = 255 - salpha;
                    uint32_t sfr = (scol >> 16) & 0xFF;
                    uint32_t sfg = (scol >> 8) & 0xFF;
                    uint32_t sfb = scol & 0xFF;

                    if (snap_zone == SNAP_LEFT)       { sx0 = 4; sy0 = 4; sw0 = scr_w/2 - 8;  sh0 = desktop_h - 8; }
                    else if (snap_zone == SNAP_RIGHT)  { sx0 = scr_w/2 + 4; sy0 = 4; sw0 = scr_w/2 - 8;  sh0 = desktop_h - 8; }
                    else /* SNAP_FULL */               { sx0 = 4; sy0 = 4; sw0 = scr_w - 8; sh0 = desktop_h - 8; }

                    /* clip to screen */
                    if (sx0 < 0) { sw0 += sx0; sx0 = 0; }
                    if (sy0 < 0) { sh0 += sy0; sy0 = 0; }
                    if (sx0 + sw0 > scr_w) sw0 = scr_w - sx0;
                    if (sy0 + sh0 > scr_h) sh0 = scr_h - sy0;
                    if (sw0 > 0 && sh0 > 0) {
                        int sr;
                        for (sr = sy0; sr < sy0 + sh0; sr++) {
                            uint32_t *p = &backbuffer[sr * scr_w + sx0];
                            int sc;
                            /* only draw border (4px edges) for subtle preview */
                            if (sr < sy0 + 3 || sr >= sy0 + sh0 - 3) {
                                for (sc = 0; sc < sw0; sc++) {
                                    uint32_t bg = p[sc];
                                    uint32_t rr = (sfr * (uint32_t)salpha + ((bg >> 16) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t gg = (sfg * (uint32_t)salpha + ((bg >> 8) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t bb = (sfb * (uint32_t)salpha + (bg & 0xFF) * (uint32_t)sia) / 255;
                                    p[sc] = (rr << 16) | (gg << 8) | bb;
                                }
                            } else {
                                /* left and right edges only */
                                for (sc = 0; sc < 3 && sc < sw0; sc++) {
                                    uint32_t bg = p[sc];
                                    uint32_t rr = (sfr * (uint32_t)salpha + ((bg >> 16) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t gg = (sfg * (uint32_t)salpha + ((bg >> 8) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t bb = (sfb * (uint32_t)salpha + (bg & 0xFF) * (uint32_t)sia) / 255;
                                    p[sc] = (rr << 16) | (gg << 8) | bb;
                                }
                                for (sc = sw0 - 3; sc < sw0; sc++) {
                                    if (sc < 0) continue;
                                    uint32_t bg = p[sc];
                                    uint32_t rr = (sfr * (uint32_t)salpha + ((bg >> 16) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t gg = (sfg * (uint32_t)salpha + ((bg >> 8) & 0xFF) * (uint32_t)sia) / 255;
                                    uint32_t bb = (sfb * (uint32_t)salpha + (bg & 0xFF) * (uint32_t)sia) / 255;
                                    p[sc] = (rr << 16) | (gg << 8) | bb;
                                }
                            }
                        }
                    }
                }

                /* 3. Draw cursor on top */
                cursor_draw(&bb_surf);

                /* 4. Wait for VBlank, then present to real framebuffer */
                vblank_wait();

                fb = (uint8_t *)fb_get_buffer();
                if (fb && fb_bpp() == 32) {
                    for (row = uy; row < uy + uh; row++)
                        memcpy(fb + row * scr_pitch + ux * 4,
                               &backbuffer[row * scr_w + ux],
                               (size_t)uw * 4);
                }

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

                /* present dirty regions to framebuffer */
                present_dirty();

                frame_end = timer_get_monotonic_us();
                ft = frame_end - frame_start;
                metrics.frame_time_us = ft;

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
