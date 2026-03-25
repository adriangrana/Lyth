#include "window.h"
#include "compositor.h"
#include "desktop.h"
#include "font_psf.h"
#include "string.h"
#include "physmem.h"

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

gui_widget_t* gui_add_label(gui_window_t* win, int x, int y,
                            const char* text, uint32_t fg) {
    gui_widget_t* w;
    int len;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    w = &win->widgets[win->widget_count++];
    memset(w, 0, sizeof(*w));
    w->type = GUI_WIDGET_LABEL;
    w->x = x; w->y = y;
    w->fg_color = fg;
    len = strlen(text);
    if (len >= 127) len = 127;
    memcpy(w->text, text, len);
    w->text[len] = '\0';
    w->width = len * GUI_FONT_W;
    w->height = GUI_FONT_H;
    return w;
}

gui_widget_t* gui_add_button(gui_window_t* win, int x, int y,
                             int w, int h, const char* text,
                             void (*on_click)(gui_widget_t*)) {
    gui_widget_t* wi;
    int len;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    wi = &win->widgets[win->widget_count++];
    memset(wi, 0, sizeof(*wi));
    wi->type = GUI_WIDGET_BUTTON;
    wi->x = x; wi->y = y;
    wi->width = w; wi->height = h;
    wi->fg_color = 0xFFFFFF;
    wi->bg_color = 0x3B82F6;
    wi->on_click = on_click;
    len = strlen(text);
    if (len >= 127) len = 127;
    memcpy(wi->text, text, len);
    wi->text[len] = '\0';
    return wi;
}

gui_widget_t* gui_add_panel(gui_window_t* win, int x, int y,
                            int w, int h, uint32_t bg) {
    gui_widget_t* wi;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    wi = &win->widgets[win->widget_count++];
    memset(wi, 0, sizeof(*wi));
    wi->type = GUI_WIDGET_PANEL;
    wi->x = x; wi->y = y;
    wi->width = w; wi->height = h;
    wi->bg_color = bg;
    return wi;
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
