/* ============================================================
 *  about.c  —  About Lyth
 *
 *  System information, version, hardware, credits.
 * ============================================================ */

#include "about.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "physmem.h"
#include "task.h"
#include "fbconsole.h"
#include "version.h"
#include "netif.h"
#include "rtc.h"
#include "video.h"
#include "input.h"

/* ---- Colours (from theme.h) ---- */
#define COL_AB_BG      THEME_COL_BASE
#define COL_AB_PANEL   THEME_COL_MANTLE
#define COL_AB_TEXT    THEME_COL_TEXT
#define COL_AB_DIM     THEME_COL_DIM
#define COL_AB_ACCENT  THEME_COL_ACCENT
#define COL_AB_LABEL   THEME_COL_SUBTEXT0
#define COL_AB_VALUE   THEME_COL_TEXT
#define COL_AB_LOGO    THEME_COL_ACCENT
#define COL_AB_BORDER  THEME_COL_BORDER
#define COL_AB_GREEN   THEME_COL_SUCCESS
#define COL_AB_YELLOW  THEME_COL_WARNING

/* ---- Layout ---- */
#define AB_WIN_W   380
#define AB_WIN_H   380
#define AB_PAD     16

/* ---- State ---- */
static gui_window_t* ab_window;
static int ab_is_open;

/* ---- Helpers ---- */

static void ab_draw_row(gui_surface_t* s, int x, int y,
                        const char* label, const char* value,
                        uint32_t lc, uint32_t vc) {
    gui_surface_draw_string(s, x, y, label, lc, 0, 0);
    gui_surface_draw_string(s, x + 120, y, value, vc, 0, 0);
}

/* ---- Drawing ---- */

static void ab_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int w = win->width;
    int y;

    if (!s->pixels) return;

    gui_surface_clear(s, COL_AB_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    y = GUI_TITLEBAR_HEIGHT + AB_PAD;

    /* Logo area */
    {
        const char* logo = LYTH_KERNEL_NAME;
        int lw = str_length(logo) * GUI_FONT_W;
        gui_surface_draw_string(s, (w - lw) / 2, y, logo, COL_AB_LOGO, 0, 0);
        y += GUI_FONT_H + 4;
    }
    {
        const char* ver = "Version " LYTH_KERNEL_VERSION;
        int vw = str_length(ver) * GUI_FONT_W;
        gui_surface_draw_string(s, (w - vw) / 2, y, ver, COL_AB_DIM, 0, 0);
        y += GUI_FONT_H + 4;
    }
    {
        const char* arch = "x86_64 kernel";
        int aw = str_length(arch) * GUI_FONT_W;
        gui_surface_draw_string(s, (w - aw) / 2, y, arch, COL_AB_DIM, 0, 0);
        y += GUI_FONT_H + 8;
    }

    /* Separator */
    gui_surface_hline(s, AB_PAD, y, w - 2 * AB_PAD, COL_AB_BORDER);
    y += 12;

    /* Section: Hardware */
    gui_surface_draw_string(s, AB_PAD, y, "Hardware", COL_AB_ACCENT, 0, 0);
    y += GUI_FONT_H + 6;

    /* Display */
    {
        char res[32];
        uint_to_str((unsigned int)fb_width(), res, sizeof(res));
        str_append(res, "x", sizeof(res));
        {
            char h[8];
            uint_to_str((unsigned int)fb_height(), h, sizeof(h));
            str_append(res, h, sizeof(res));
        }
        str_append(res, "x", sizeof(res));
        {
            char b[8];
            uint_to_str((unsigned int)fb_bpp(), b, sizeof(b));
            str_append(res, b, sizeof(res));
        }
        ab_draw_row(s, AB_PAD + 8, y, "Display:", res, COL_AB_LABEL, COL_AB_VALUE);
        y += GUI_FONT_H + 2;
    }

    /* Video backend */
    {
        const char* vname = video_backend_name();
        if (vname && vname[0]) {
            ab_draw_row(s, AB_PAD + 8, y, "Video:", vname, COL_AB_LABEL, COL_AB_VALUE);
            y += GUI_FONT_H + 2;
        }
    }

    /* Memory */
    {
        char mem[32];
        unsigned int total = physmem_total_bytes() / 1024;  /* KB */
        unsigned int free_kb = physmem_free_bytes() / 1024;
        unsigned int used_kb = total - free_kb;
        uint_to_str(used_kb / 1024, mem, sizeof(mem));
        str_append(mem, " / ", sizeof(mem));
        {
            char t[8];
            uint_to_str(total / 1024, t, sizeof(t));
            str_append(mem, t, sizeof(mem));
        }
        str_append(mem, " MB", sizeof(mem));
        ab_draw_row(s, AB_PAD + 8, y, "Memory:", mem, COL_AB_LABEL, COL_AB_VALUE);
        y += GUI_FONT_H + 2;
    }

    /* Network */
    {
        netif_t* iface = netif_get_default();
        if (iface) {
            char net_info[32];
            if (iface->ip_addr) {
                uint8_t* ip = (uint8_t*)&iface->ip_addr;
                net_info[0] = '\0';
                {
                    char o[4];
                    int p;
                    for (p = 0; p < 4; p++) {
                        uint_to_str(ip[p], o, sizeof(o));
                        str_append(net_info, o, sizeof(net_info));
                        if (p < 3) str_append(net_info, ".", sizeof(net_info));
                    }
                }
                ab_draw_row(s, AB_PAD + 8, y, "Network:", net_info, COL_AB_LABEL, COL_AB_GREEN);
            } else {
                ab_draw_row(s, AB_PAD + 8, y, "Network:", "Not configured", COL_AB_LABEL, COL_AB_YELLOW);
            }
        } else {
            ab_draw_row(s, AB_PAD + 8, y, "Network:", "No interface", COL_AB_LABEL, COL_AB_DIM);
        }
        y += GUI_FONT_H + 2;
    }

    /* Separator */
    y += 4;
    gui_surface_hline(s, AB_PAD, y, w - 2 * AB_PAD, COL_AB_BORDER);
    y += 12;

    /* Section: System */
    gui_surface_draw_string(s, AB_PAD, y, "System", COL_AB_ACCENT, 0, 0);
    y += GUI_FONT_H + 6;

    /* Uptime */
    {
        char up[32];
        unsigned int ms = timer_get_uptime_ms();
        unsigned int sec = ms / 1000;
        unsigned int hr = sec / 3600;
        unsigned int mn = (sec % 3600) / 60;
        unsigned int sc = sec % 60;

        uint_to_str(hr, up, sizeof(up));
        str_append(up, "h ", sizeof(up));
        {
            char m[4];
            uint_to_str(mn, m, sizeof(m));
            str_append(up, m, sizeof(up));
        }
        str_append(up, "m ", sizeof(up));
        {
            char ss[4];
            uint_to_str(sc, ss, sizeof(ss));
            str_append(up, ss, sizeof(up));
        }
        str_append(up, "s", sizeof(up));
        ab_draw_row(s, AB_PAD + 8, y, "Uptime:", up, COL_AB_LABEL, COL_AB_VALUE);
        y += GUI_FONT_H + 2;
    }

    /* Processes */
    {
        char procs[8];
        {
            task_snapshot_t snaps[16];
            int n = task_list(snaps, 16);
            uint_to_str((unsigned int)n, procs, sizeof(procs));
        }
        ab_draw_row(s, AB_PAD + 8, y, "Processes:", procs, COL_AB_LABEL, COL_AB_VALUE);
        y += GUI_FONT_H + 2;
    }

    /* Build flavor */
    ab_draw_row(s, AB_PAD + 8, y, "Build:", LYTH_KERNEL_BUILD_FLAVOR, COL_AB_LABEL, COL_AB_DIM);
    y += GUI_FONT_H + 2;

    /* Date */
    {
        rtc_time_t rt;
        char date[24];
        rtc_read(&rt);
        /* YYYY-MM-DD */
        uint_to_str(2000 + rt.year, date, sizeof(date));
        str_append(date, "-", sizeof(date));
        if (rt.month < 10) str_append(date, "0", sizeof(date));
        {
            char m[4];
            uint_to_str(rt.month, m, sizeof(m));
            str_append(date, m, sizeof(date));
        }
        str_append(date, "-", sizeof(date));
        if (rt.day < 10) str_append(date, "0", sizeof(date));
        {
            char d[4];
            uint_to_str(rt.day, d, sizeof(d));
            str_append(date, d, sizeof(date));
        }
        ab_draw_row(s, AB_PAD + 8, y, "Date:", date, COL_AB_LABEL, COL_AB_VALUE);
        y += GUI_FONT_H + 2;
    }

    /* Separator */
    y += 4;
    gui_surface_hline(s, AB_PAD, y, w - 2 * AB_PAD, COL_AB_BORDER);
    y += 12;

    /* Credits */
    {
        const char* line = "Lyth OS - A minimal x86_64 operating system";
        int lw = str_length(line) * GUI_FONT_W;
        gui_surface_draw_string(s, (w - lw) / 2, y, line, COL_AB_DIM, 0, 0);
    }
}

/* ---- Events ---- */

static void ab_on_key(gui_window_t* win, int event_type, char key) {
    (void)win; (void)key;
    if (event_type == INPUT_EVENT_CHAR && key == 'r') {
        gui_window_invalidate(ab_window);
    }
}

static void ab_on_close(gui_window_t* win) {
    ab_is_open = 0;
    ab_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

/* ---- Public API ---- */

void about_app_open(void) {
    if (ab_is_open && ab_window) {
        gui_window_focus(ab_window);
        gui_dirty_add(ab_window->x, ab_window->y,
                      ab_window->width, ab_window->height);
        gui_window_invalidate(ab_window);
        return;
    }

    ab_window = gui_window_create("About Lyth", 260, 80, AB_WIN_W, AB_WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!ab_window) return;

    ab_window->on_paint = ab_paint;
    ab_window->on_key = ab_on_key;
    ab_window->on_close = ab_on_close;

    ab_is_open = 1;
}
