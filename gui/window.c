#include "window.h"
#include "compositor.h"
#include "desktop.h"
#include "font_psf.h"
#include "theme.h"
#include "string.h"
#include "physmem.h"
#include "timer.h"

static gui_window_t windows[GUI_MAX_WINDOWS];
static int window_used[GUI_MAX_WINDOWS];
static int next_window_id = 1;

/* ------------------------------------------------------------------ */
/*  Surface operations                                                 */
/* ------------------------------------------------------------------ */

void gui_surface_alloc(gui_surface_t* s, int w, int h) {
    uint32_t size;
    uint32_t phys;
    if (!s || w <= 0 || h <= 0) return;
    size = (uint32_t)(w * h) * 4;
    phys = physmem_alloc_region(size, 4096);
    if (!phys) {
        s->pixels = 0;
        s->width = 0;
        s->height = 0;
        s->stride = 0;
        s->alloc_phys = 0;
        s->alloc_size = 0;
        return;
    }
    s->pixels = (uint32_t*)(uintptr_t)phys;
    s->width = w;
    s->height = h;
    s->stride = w;
    s->alloc_phys = phys;
    s->alloc_size = size;
    memset(s->pixels, 0, size);
}

void gui_surface_free(gui_surface_t* s) {
    if (s && s->alloc_phys) {
        physmem_free_region(s->alloc_phys, s->alloc_size);
        s->pixels = 0;
        s->alloc_phys = 0;
        s->alloc_size = 0;
    }
}

void gui_surface_clear(gui_surface_t* s, uint32_t c) {
    if (s && s->pixels) {
        memset32(s->pixels, c, (size_t)(s->width * s->height));
    }
}

void gui_surface_fill(gui_surface_t* s, int x, int y, int w, int h, uint32_t c) {
    int x0, y0, x1, y1, span;
    if (!s || !s->pixels) return;
    x0 = x < 0 ? 0 : x;
    y0 = y < 0 ? 0 : y;
    x1 = x + w > s->width ? s->width : x + w;
    y1 = y + h > s->height ? s->height : y + h;
    span = x1 - x0;
    if (span <= 0 || y0 >= y1) return;
    for (int py = y0; py < y1; py++) {
        memset32(&s->pixels[py * s->stride + x0], c, (size_t)span);
    }
}

void gui_surface_hline(gui_surface_t* s, int x, int y, int w, uint32_t c) {
    int x0, x1, span;
    if (!s || !s->pixels || w <= 0 || y < 0 || y >= s->height) return;
    x0 = x < 0 ? 0 : x;
    x1 = x + w > s->width ? s->width : x + w;
    span = x1 - x0;
    if (span <= 0) return;
    memset32(&s->pixels[y * s->stride + x0], c, (size_t)span);
}

void gui_surface_putpixel(gui_surface_t* s, int x, int y, uint32_t c) {
    if (s && s->pixels && x >= 0 && y >= 0 && x < s->width && y < s->height) {
        s->pixels[y * s->stride + x] = c;
    }
}

void gui_surface_draw_char(gui_surface_t* s, int x, int y, unsigned char ch,
                           uint32_t fg, uint32_t bg, int draw_bg) {
    int row, col;
    uint8_t bits;
    if (!s || !s->pixels) return;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    /* skip if entirely out of bounds */
    if (x + FONT_PSF_WIDTH <= 0 || x >= s->width ||
        y + FONT_PSF_HEIGHT <= 0 || y >= s->height) return;
    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= s->height) continue;
        bits = font_psf_data[ch][row];
        uint32_t* dst = &s->pixels[py * s->stride];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= s->width) continue;
            if (bits & (0x80u >> col)) {
                dst[px] = fg;
            } else if (draw_bg) {
                dst[px] = bg;
            }
        }
    }
}

void gui_surface_draw_string(gui_surface_t* s, int x, int y, const char* str,
                             uint32_t fg, uint32_t bg, int draw_bg) {
    if (!str) return;
    while (*str) {
        gui_surface_draw_char(s, x, y, (unsigned char)*str, fg, bg, draw_bg);
        x += FONT_PSF_WIDTH;
        str++;
    }
}

void gui_surface_draw_string_n(gui_surface_t* s, int x, int y, const char* str,
                               int max_chars, uint32_t fg, uint32_t bg, int draw_bg) {
    int i = 0;
    if (!str) return;
    while (*str && i < max_chars) {
        gui_surface_draw_char(s, x, y, (unsigned char)*str, fg, bg, draw_bg);
        x += FONT_PSF_WIDTH;
        str++;
        i++;
    }
}

void gui_surface_blit(gui_surface_t* dst, int dx, int dy,
                      const gui_surface_t* src, int sx, int sy, int w, int h) {
    int row;
    if (!dst || !dst->pixels || !src || !src->pixels) return;
    /* clip */
    if (sx < 0) { dx -= sx; w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; h += sy; sy = 0; }
    if (dx < 0) { sx -= dx; w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; h += dy; dy = 0; }
    if (sx + w > src->width)  w = src->width - sx;
    if (sy + h > src->height) h = src->height - sy;
    if (dx + w > dst->width)  w = dst->width - dx;
    if (dy + h > dst->height) h = dst->height - dy;
    if (w <= 0 || h <= 0) return;
    for (row = 0; row < h; row++) {
        memcpy(&dst->pixels[(dy + row) * dst->stride + dx],
               &src->pixels[(sy + row) * src->stride + sx],
               (size_t)w * 4);
    }
}

/* ------------------------------------------------------------------ */
/*  Centralized window decorations                                     */
/* ------------------------------------------------------------------ */

/*
 * Draw a filled circle (close/minimize/maximize buttons).
 * cx, cy = center in surface coords; r = radius.
 */
static void draw_circle(gui_surface_t* s, int cx, int cy, int r, uint32_t col)
{
    int dy, dx;
    int r2 = r * r;
    for (dy = -r; dy <= r; dy++) {
        for (dx = -r; dx <= r; dx++) {
            if (dx * dx + dy * dy <= r2)
                gui_surface_putpixel(s, cx + dx, cy + dy, col);
        }
    }
}

/*
 * gui_window_draw_decorations — draw titlebar, traffic-light buttons,
 * title text, and border separator.  Call this at the start of on_paint
 * (after gui_surface_clear) for any decorated window.
 *
 * Renders:
 *   - Titlebar background (active/inactive)
 *   - Traffic-light close/minimize/maximize circles (left side, macOS-style)
 *   - Title text (centered)
 *   - Subtle separator line at titlebar bottom
 */
void gui_window_draw_decorations(gui_window_t* win)
{
    gui_surface_t* s;
    int w, focused, title_len, title_x, text_y;
    uint32_t tb_bg, tb_text, sep_col;
    int btn_y, btn_r, btn_pad, btn_x;

    if (!win) return;
    if (win->flags & GUI_WIN_NO_DECOR) return;

    s = &win->surface;
    if (!s->pixels) return;

    w = win->width;
    focused = (win->flags & GUI_WIN_FOCUSED) ? 1 : 0;

    /* Titlebar background */
    tb_bg   = focused ? theme.titlebar : theme.titlebar_inactive;
    tb_text = focused ? theme.titlebar_text : theme.titlebar_text_dim;
    sep_col = THEME_COL_TITLEBAR_BORDER;

    /* Rounded top corners for titlebar (radius 3) */
    {
        int r = THEME_WIN_RADIUS;
        if (r >= 3) {
            gui_surface_fill(s, 3, 0, w - 6, 1, tb_bg);
            gui_surface_fill(s, 2, 1, w - 4, 1, tb_bg);
            gui_surface_fill(s, 1, 2, w - 2, 1, tb_bg);
            gui_surface_fill(s, 0, 3, w, GUI_TITLEBAR_HEIGHT - 3, tb_bg);
        } else {
            gui_surface_fill(s, 0, 0, w, GUI_TITLEBAR_HEIGHT, tb_bg);
        }
    }

    /* ---- Traffic-light buttons (left side) ---- */
    btn_y   = GUI_TITLEBAR_HEIGHT / 2;
    btn_r   = 5;
    btn_pad = 20;      /* center-to-center distance */
    btn_x   = 16;      /* first button X center */

    /* Close (red) */
    if (win->flags & GUI_WIN_CLOSEABLE) {
        draw_circle(s, btn_x, btn_y, btn_r,
                    focused ? THEME_COL_CLOSE : THEME_COL_OVERLAY0);
    }
    /* Minimize (yellow) */
    draw_circle(s, btn_x + btn_pad, btn_y, btn_r,
                focused ? THEME_COL_MINIMIZE : THEME_COL_OVERLAY0);
    /* Maximize (green) */
    draw_circle(s, btn_x + btn_pad * 2, btn_y, btn_r,
                focused ? THEME_COL_MAXIMIZE : THEME_COL_OVERLAY0);

    /* ---- Title text (centered) ---- */
    title_len = 0;
    { const char* p = win->title; while (*p) { title_len++; p++; } }
    title_x = (w - title_len * FONT_PSF_WIDTH) / 2;
    if (title_x < btn_x + btn_pad * 3)
        title_x = btn_x + btn_pad * 3;   /* don't overlap buttons */
    text_y = (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2;
    gui_surface_draw_string(s, title_x, text_y, win->title, tb_text, 0, 0);

    /* ---- Separator line ---- */
    gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, w, sep_col);

    /* ---- Resize grip (bottom-right corner, 3 diagonal dots) ---- */
    if (win->flags & GUI_WIN_RESIZABLE) {
        uint32_t grip_col = focused ? THEME_COL_OVERLAY1 : THEME_COL_OVERLAY0;
        int h = win->height;
        /* three small diagonal lines */
        gui_surface_putpixel(s, w - 4, h - 2, grip_col);

        gui_surface_putpixel(s, w - 7, h - 2, grip_col);
        gui_surface_putpixel(s, w - 4, h - 5, grip_col);

        gui_surface_putpixel(s, w - 10, h - 2, grip_col);
        gui_surface_putpixel(s, w - 7, h - 5, grip_col);
        gui_surface_putpixel(s, w - 4, h - 8, grip_col);
    }
}

/* ------------------------------------------------------------------ */
/*  Window management                                                  */
/* ------------------------------------------------------------------ */

gui_window_t* gui_window_create(const char* title, int x, int y,
                                int w, int h, uint32_t flags) {
    int i;
    gui_window_t* win;
    int len;

    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!window_used[i]) {
            win = &windows[i];
            memset(win, 0, sizeof(*win));
            win->id = next_window_id++;
            win->x = x;
            win->y = y;
            win->width = w;
            win->height = h;
            win->flags = flags;
            win->z_order = i;
            win->alpha = 0;    /* start invisible, fade in */
            win->anim_alpha_target = 255;
            win->anim_closing = 0;
            win->anim_minimizing = 0;
            win->anim_alpha_start = 0;
            win->anim_start_ms = timer_get_uptime_ms();
            win->anim_dur_ms = THEME_ANIM_NORMAL;
            win->redraw_count = 0;
            win->needs_redraw = 1;

            len = strlen(title);
            if (len >= GUI_MAX_TITLE) len = GUI_MAX_TITLE - 1;
            memcpy(win->title, title, len);
            win->title[len] = '\0';

            /* allocate per-window surface */
            gui_surface_alloc(&win->surface, w, h);

            window_used[i] = 1;

            /* bring to front and focus */
            gui_window_focus(win);

            /* ensure the full window is composited on next frame */
            gui_dirty_add(x, y, w, h);

            /* taskbar needs to show the new window item */
            desktop_invalidate_taskbar();

            return win;
        }
    }
    return 0;
}

void gui_window_destroy(gui_window_t* win) {
    int i;
    if (!win) return;
    /* dirty the area so desktop underneath gets recomposed */
    gui_dirty_add(win->x, win->y, win->width, win->height);
    /* taskbar needs to remove the window item */
    desktop_invalidate_taskbar();
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i] && &windows[i] == win) {
            gui_surface_free(&win->surface);
            window_used[i] = 0;
            break;
        }
    }
}

void gui_window_close_animated(gui_window_t* win) {
    if (!win) return;
    if (win->anim_closing) return;  /* already fading out */
    win->anim_alpha_start = win->alpha;
    win->anim_alpha_target = 0;
    win->anim_closing = 1;
    win->anim_start_ms = timer_get_uptime_ms();
    win->anim_dur_ms = THEME_ANIM_NORMAL;
}

/*
 * Ease-out cubic: f(t) = 1 - (1-t)^3
 * Input/output in 0..255 fixed-point (t_fp = progress 0..255).
 * Returns interpolated value 0..255.
 */
static uint8_t ease_out_cubic_u8(int t_fp) {
    int inv, inv2, inv3, result;
    if (t_fp <= 0) return 0;
    if (t_fp >= 255) return 255;
    inv = 255 - t_fp;               /* (1 - t) in 0..255 */
    inv2 = (inv * inv) >> 8;        /* (1 - t)^2 */
    inv3 = (inv2 * inv) >> 8;       /* (1 - t)^3 */
    result = 255 - inv3;
    return (uint8_t)(result < 0 ? 0 : (result > 255 ? 255 : result));
}

/*
 * Start or redirect an animation. Records current alpha as start,
 * sets target and duration, resets the clock.
 */
void gui_window_anim_start(gui_window_t *w, uint8_t target, unsigned int dur_ms) {
    if (!w) return;
    w->anim_alpha_start = w->alpha;
    w->anim_alpha_target = target;
    w->anim_start_ms = timer_get_uptime_ms();
    w->anim_dur_ms = dur_ms ? dur_ms : THEME_ANIM_NORMAL;
}

void gui_window_anim_tick(void) {
    int i;
    unsigned int now = timer_get_uptime_ms();
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        gui_window_t *w;
        unsigned int elapsed, dur;
        int t_fp, a_start, a_target, new_alpha;

        if (!window_used[i]) continue;
        w = &windows[i];
        if (w->alpha == w->anim_alpha_target) continue;

        dur = w->anim_dur_ms;
        if (dur == 0) dur = THEME_ANIM_NORMAL;

        elapsed = now - w->anim_start_ms;
        if (elapsed >= dur) {
            /* animation complete */
            w->alpha = w->anim_alpha_target;
        } else {
            /* progress 0..255 fixed-point */
            t_fp = (int)(elapsed * 255 / dur);
            /* ease-out cubic */
            t_fp = ease_out_cubic_u8(t_fp);
            /* interpolate alpha */
            a_start = (int)w->anim_alpha_start;
            a_target = (int)w->anim_alpha_target;
            new_alpha = a_start + ((a_target - a_start) * t_fp) / 255;
            if (new_alpha < 0) new_alpha = 0;
            if (new_alpha > 255) new_alpha = 255;
            w->alpha = (uint8_t)new_alpha;
        }

        gui_dirty_add(w->x - 14, w->y - 14,
                       w->width + 28, w->height + 28);

        /* if closing fade finished, destroy the window */
        if (w->anim_closing && w->alpha == 0) {
            if (w->on_close) w->on_close(w);
            gui_window_destroy(w);
            continue;
        }

        /* if minimize fade finished, set minimized flag */
        if (w->anim_minimizing && w->alpha == 0) {
            w->flags |= GUI_WIN_MINIMIZED;
            w->anim_minimizing = 0;
            w->alpha = 255;             /* restore alpha for when un-minimized */
            w->anim_alpha_target = 255;
            desktop_invalidate_taskbar();
        }
    }
}

void gui_window_focus(gui_window_t* win) {
    int i, max_z;
    if (!win) return;
    max_z = 0;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i] && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
        if (window_used[i])
            windows[i].flags &= ~GUI_WIN_FOCUSED;
    }
    win->flags |= GUI_WIN_FOCUSED;
    win->z_order = max_z + 1;
}

void gui_window_move(gui_window_t* win, int x, int y) {
    int old_x, old_y;

    if (!win) return;

    old_x = win->x;
    old_y = win->y;

    if (old_x == x && old_y == y) return;

    win->x = x;
    win->y = y;

    /* invalidar zona vieja y zona nueva */
    gui_dirty_add(old_x, old_y, win->width, win->height);
    gui_dirty_add(win->x, win->y, win->width, win->height);
}

void gui_window_resize(gui_window_t* win, int new_w, int new_h) {
    int old_w, old_h;
    uint32_t new_size, new_phys;

    if (!win) return;
    if (new_w < GUI_RESIZE_MIN_W) new_w = GUI_RESIZE_MIN_W;
    if (new_h < GUI_RESIZE_MIN_H) new_h = GUI_RESIZE_MIN_H;

    old_w = win->width;
    old_h = win->height;
    if (old_w == new_w && old_h == new_h) return;

    /* dirty old area (including shadow) */
    gui_dirty_add(win->x - 1, win->y - 1,
                  old_w + THEME_SHADOW_EXTENT + 2,
                  old_h + THEME_SHADOW_EXTENT + 2);

    /* reallocate surface */
    new_size = (uint32_t)(new_w * new_h) * 4;
    new_phys = physmem_alloc_region(new_size, 4096);
    if (!new_phys) return; /* keep old size on failure */

    /* free old surface */
    gui_surface_free(&win->surface);

    /* set new surface */
    win->surface.pixels = (uint32_t*)(uintptr_t)new_phys;
    win->surface.width = new_w;
    win->surface.height = new_h;
    win->surface.stride = new_w;
    win->surface.alloc_phys = new_phys;
    win->surface.alloc_size = new_size;
    memset(win->surface.pixels, 0, new_size);

    win->width = new_w;
    win->height = new_h;

    /* redraw at new size */
    win->needs_redraw = 1;
    gui_dirty_add(win->x, win->y,
                  new_w + THEME_SHADOW_EXTENT,
                  new_h + THEME_SHADOW_EXTENT);
}

void gui_window_invalidate(gui_window_t* win) {
    if (win) {
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

int gui_window_content_x(gui_window_t* win) {
    if (win->flags & GUI_WIN_NO_DECOR) return win->x;
    return win->x + GUI_BORDER_WIDTH;
}

int gui_window_content_y(gui_window_t* win) {
    if (win->flags & GUI_WIN_NO_DECOR) return win->y;
    return win->y + GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH;
}

int gui_window_content_w(gui_window_t* win) {
    if (win->flags & GUI_WIN_NO_DECOR) return win->width;
    return win->width - GUI_BORDER_WIDTH * 2;
}

int gui_window_content_h(gui_window_t* win) {
    if (win->flags & GUI_WIN_NO_DECOR) return win->height;
    return win->height - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH * 2;
}

int gui_window_count(void) {
    int i, c = 0;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i]) c++;
    }
    return c;
}

gui_window_t* gui_window_get(int index) {
    int i, c = 0;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i]) {
            if (c == index) return &windows[i];
            c++;
        }
    }
    return 0;
}
