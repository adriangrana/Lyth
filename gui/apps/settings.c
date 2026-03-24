/*
 * Settings app — shows current display resolution, GPU/framebuffer info,
 * and frame metrics (FPS, render/compose/present times).
 *
 * Resolution change through VBE is not available at runtime with the
 * current multiboot framebuffer backend, so we show info + debug overlay.
 * When a real VBE/GOP mode-set is added, this app will drive it.
 */

#include "settings.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "fbconsole.h"
#include "video.h"

#define COL_SET_BG     0x1E1E2E
#define COL_SET_TEXT   0xCDD6F4
#define COL_SET_DIM    0x6C7086
#define COL_SET_ACCENT 0x89B4FA
#define COL_SET_LABEL  0xA6ADC8
#define COL_SET_GOOD   0xA6E3A1
#define COL_SET_WARN   0xF9E2AF

static gui_window_t* set_window;
static int set_open;

static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static void set_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int ox = GUI_BORDER_WIDTH + 12;
    int oy = GUI_TITLEBAR_HEIGHT + 12;
    int row_h = FONT_PSF_HEIGHT + 6;
    char buf[64];
    gui_metrics_t met;

    if (!s->pixels) return;

    gui_surface_clear(s, COL_SET_BG);

    /* title bar */
    gui_surface_fill(s, 0, 0, win->width, GUI_TITLEBAR_HEIGHT, 0x181825);
    {
        int cx = win->width - 20, cy = GUI_TITLEBAR_HEIGHT / 2, r;
        for (r = -5; r <= 5; r++) {
            int dx;
            for (dx = -5; dx <= 5; dx++) {
                if (r * r + dx * dx <= 25)
                    gui_surface_putpixel(s, cx + dx, cy + r, 0xF38BA8);
            }
        }
    }
    gui_surface_draw_string(s, 10, (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2,
                            win->title, 0xCDD6F4, 0, 0);
    gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, win->width, 0x313244);

    /* section: Display */
    gui_surface_draw_string(s, ox, oy, "Display", COL_SET_ACCENT, 0, 0);
    oy += row_h + 2;
    gui_surface_hline(s, ox, oy, win->width - ox * 2, 0x313244);
    oy += 6;

    /* resolution */
    {
        int pos = 0;
        int_to_str(fb_width(), buf, sizeof(buf)); pos = (int)strlen(buf);
        buf[pos++] = ' '; buf[pos++] = 'x'; buf[pos++] = ' ';
        int_to_str(fb_height(), buf + pos, sizeof(buf) - pos); pos += (int)strlen(buf + pos);
        buf[pos] = '\0';
    }
    gui_surface_draw_string(s, ox + 8, oy, "Resolution:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* bpp */
    int_to_str(fb_bpp(), buf, sizeof(buf));
    {
        int pos = (int)strlen(buf);
        memcpy(buf + pos, " bpp", 5);
    }
    gui_surface_draw_string(s, ox + 8, oy, "Color depth:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* backend */
    gui_surface_draw_string(s, ox + 8, oy, "Backend:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, video_backend_name(), COL_SET_TEXT, 0, 0);
    oy += row_h + 8;

    /* section: Performance */
    gui_surface_draw_string(s, ox, oy, "Performance", COL_SET_ACCENT, 0, 0);
    oy += row_h + 2;
    gui_surface_hline(s, ox, oy, win->width - ox * 2, 0x313244);
    oy += 6;

    gui_get_metrics(&met);

    /* Presents/s */
    int_to_str(met.fps, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " /s", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Presents:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf,
                            met.fps >= 50 ? COL_SET_GOOD : COL_SET_WARN, 0, 0);
    oy += row_h;

    /* Last frame time */
    int_to_str(met.frame_time_us, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Last frame:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* Avg frame time */
    int_to_str(met.frame_time_avg, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Avg frame:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* Max frame time */
    int_to_str(met.frame_time_max, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Max frame:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf,
                            met.frame_time_max > 16000 ? COL_SET_WARN : COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* compose time */
    int_to_str(met.compose_us, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Compose:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* present time */
    int_to_str(met.present_us, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Present:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* dirty rects */
    int_to_str(met.dirty_count, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " rects", 7); }
    gui_surface_draw_string(s, ox + 8, oy, "Dirty:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* pixels */
    int_to_str(met.pixels_copied, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " px", 4); }
    gui_surface_draw_string(s, ox + 8, oy, "Pixels:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_TEXT, 0, 0);
    oy += row_h;

    /* drag status */
    gui_surface_draw_string(s, ox + 8, oy, "Dragging:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy,
                            met.drag_active ? "YES" : "no", 
                            met.drag_active ? COL_SET_WARN : COL_SET_DIM, 0, 0);
    oy += row_h;

    /* coalesced moves */
    int_to_str(met.coalesced_moves, buf, sizeof(buf));
    gui_surface_draw_string(s, ox + 8, oy, "Coalesced:", COL_SET_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, buf, COL_SET_DIM, 0, 0);
    oy += row_h + 8;

    gui_surface_draw_string(s, ox, oy,
        "Press R to refresh metrics.", COL_SET_DIM, 0, 0);
}

static void set_on_key(gui_window_t* win, int event_type, char key) {
    (void)event_type;
    if (key == 'r' || key == 'R') {
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

static void set_on_close(gui_window_t* win) {
    set_open = 0;
    set_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

void settings_app_open(void) {
    if (set_open && set_window) {
        gui_window_focus(set_window);
        gui_dirty_add(set_window->x, set_window->y,
                      set_window->width, set_window->height);
        return;
    }

    set_window = gui_window_create("Settings", 240, 60, 380, 520,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!set_window) return;

    set_window->on_paint = set_paint;
    set_window->on_key = set_on_key;
    set_window->on_close = set_on_close;

    set_open = 1;
    gui_window_focus(set_window);
    gui_dirty_add(set_window->x, set_window->y,
                  set_window->width, set_window->height);
}
