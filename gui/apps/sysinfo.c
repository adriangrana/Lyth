/*
 * System Info app — shows OS name, arch, display mode, memory, uptime.
 */

#include "sysinfo.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "fbconsole.h"
#include "physmem.h"
#include "task.h"

#define COL_SI_BG      THEME_COL_BASE
#define COL_SI_TEXT    THEME_COL_TEXT
#define COL_SI_DIM     THEME_COL_DIM
#define COL_SI_ACCENT  THEME_COL_ACCENT
#define COL_SI_LABEL   THEME_COL_SUBTEXT0

static gui_window_t* si_window;
static int si_open;

static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static void draw_info_row(gui_surface_t* s, int x, int y, const char* label, const char* value) {
    gui_surface_draw_string(s, x, y, label, COL_SI_LABEL, 0, 0);
    gui_surface_draw_string(s, x + 120, y, value, COL_SI_TEXT, 0, 0);
}

static void si_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int ox = GUI_BORDER_WIDTH + 12;
    int oy = GUI_TITLEBAR_HEIGHT + 12;
    int row_h = THEME_LH_LOOSE;
    char buf[64];

    if (!s->pixels) return;

    gui_surface_clear(s, COL_SI_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    /* OS name */
    gui_surface_draw_string(s, ox, oy, "Lyth OS", COL_SI_ACCENT, 0, 0);
    oy += row_h + 4;
    gui_surface_hline(s, ox, oy, win->width - ox * 2, THEME_COL_BORDER);
    oy += 8;

    draw_info_row(s, ox, oy, "Architecture:", "x86_64"); oy += row_h;

    /* display mode */
    {
        int pos = 0;
        int_to_str(fb_width(), buf, sizeof(buf));
        pos = (int)strlen(buf);
        buf[pos++] = 'x';
        int_to_str(fb_height(), buf + pos, sizeof(buf) - pos);
        pos += (int)strlen(buf + pos);
        buf[pos++] = 'x';
        int_to_str(fb_bpp(), buf + pos, sizeof(buf) - pos);
    }
    draw_info_row(s, ox, oy, "Display:", buf); oy += row_h;

    /* memory */
    {
        int_to_str(physmem_total_bytes() / (1024 * 1024), buf, sizeof(buf));
        int pos = (int)strlen(buf);
        memcpy(buf + pos, " MB total", 10);
    }
    draw_info_row(s, ox, oy, "Memory:", buf); oy += row_h;

    /* uptime */
    {
        unsigned int ms = timer_get_uptime_ms();
        unsigned int secs = ms / 1000;
        unsigned int mins = secs / 60;
        unsigned int hrs = mins / 60;
        int pos = 0;

        int_to_str(hrs, buf, sizeof(buf));
        pos = (int)strlen(buf);
        buf[pos++] = 'h'; buf[pos++] = ' ';
        int_to_str(mins % 60, buf + pos, sizeof(buf) - pos);
        pos += (int)strlen(buf + pos);
        buf[pos++] = 'm'; buf[pos++] = ' ';
        int_to_str(secs % 60, buf + pos, sizeof(buf) - pos);
        pos += (int)strlen(buf + pos);
        buf[pos++] = 's'; buf[pos] = '\0';
    }
    draw_info_row(s, ox, oy, "Uptime:", buf); oy += row_h;

    /* processes */
    {
        task_snapshot_t snaps[32];
        int count = task_list(snaps, 32);
        int_to_str(count, buf, sizeof(buf));
        int pos = (int)strlen(buf);
        memcpy(buf + pos, " processes", 11);
    }
    draw_info_row(s, ox, oy, "Tasks:", buf); oy += row_h;
}

static void si_on_close(gui_window_t* win) {
    si_open = 0;
    si_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

void sysinfo_app_open(void) {
    if (si_open && si_window) {
        gui_window_focus(si_window);
        gui_dirty_add(si_window->x, si_window->y,
                      si_window->width, si_window->height);
        return;
    }

    si_window = gui_window_create("System Info", 300, 120, 360, 260,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!si_window) return;

    si_window->on_paint = si_paint;
    si_window->on_close = si_on_close;

    si_open = 1;
    gui_window_focus(si_window);
    gui_dirty_add(si_window->x, si_window->y,
                  si_window->width, si_window->height);
}
