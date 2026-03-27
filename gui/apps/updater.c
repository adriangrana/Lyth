/* ============================================================
 *  updater.c  —  System Updates
 *
 *  Shows current version, simulates checking for updates,
 *  displays changelog / update history.
 * ============================================================ */

#include "updater.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "version.h"
#include "audio.h"
#include "input.h"

/* ---- Colours ---- */
#define COL_BG      THEME_COL_BASE
#define COL_PANEL   THEME_COL_MANTLE
#define COL_TEXT    THEME_COL_TEXT
#define COL_DIM     THEME_COL_DIM
#define COL_ACCENT  THEME_COL_ACCENT
#define COL_LABEL   THEME_COL_SUBTEXT0
#define COL_BORDER  THEME_COL_BORDER
#define COL_GREEN   THEME_COL_SUCCESS
#define COL_YELLOW  THEME_COL_WARNING

/* ---- Layout ---- */
#define UPD_WIN_W   420
#define UPD_WIN_H   460
#define PAD         16

/* ---- Update check states ---- */
#define ST_IDLE        0
#define ST_CHECKING    1
#define ST_UP_TO_DATE  2
#define ST_DOWNLOADING 3
#define ST_INSTALLED   4

/* ---- Changelog entries ---- */
typedef struct {
    const char* version;
    const char* date;
    const char* summary;
} changelog_entry_t;

static const changelog_entry_t changelog[] = {
    { "0.6.0", "2025-06-27", "Audio driver, browser, WiFi, system sounds" },
    { "0.5.0", "2025-06-15", "GUI compositor, window manager, desktop shell" },
    { "0.4.0", "2025-05-20", "FAT32 filesystem, file manager app" },
    { "0.3.0", "2025-04-10", "TCP/IP stack, DNS resolver, DHCP client" },
    { "0.2.0", "2025-03-01", "PS/2 keyboard+mouse, VGA framebuffer" },
    { "0.1.0", "2025-01-15", "Boot, GDT, IDT, physical/virtual memory" },
};
#define CHANGELOG_COUNT 6

/* ---- State ---- */
static gui_window_t* upd_window;
static int upd_is_open;
static int upd_state;
static int upd_progress;
static unsigned int upd_check_start;

/* ---- Helpers ---- */

static void upd_draw_card(gui_surface_t* s, int x, int y, int w, int h,
                           uint32_t bg, uint32_t border) {
    int i;
    gui_surface_fill(s, x, y, w, h, bg);
    gui_surface_hline(s, x, y, w, border);
    gui_surface_hline(s, x, y + h - 1, w, border);
    for (i = 0; i < h; i++) {
        gui_surface_putpixel(s, x, y + i, border);
        gui_surface_putpixel(s, x + w - 1, y + i, border);
    }
}

static void upd_advance(void) {
    if (upd_state == ST_CHECKING) {
        unsigned int elapsed = timer_get_ticks() - upd_check_start;
        upd_progress = (int)((elapsed * 100u) / 300u);
        if (upd_progress >= 100) {
            upd_progress = 100;
            upd_state = ST_UP_TO_DATE;
            audio_play_sound(SND_NOTIFY);
        }
    } else if (upd_state == ST_DOWNLOADING) {
        unsigned int elapsed = timer_get_ticks() - upd_check_start;
        upd_progress = (int)((elapsed * 100u) / 500u);
        if (upd_progress >= 100) {
            upd_progress = 100;
            upd_state = ST_INSTALLED;
            audio_play_sound(SND_NOTIFY);
        }
    }
}

/* ---- Paint ---- */

static void upd_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int w = win->width;
    int y;

    if (!s->pixels) return;

    upd_advance();
    if (upd_state == ST_CHECKING || upd_state == ST_DOWNLOADING)
        win->needs_redraw = 1;

    gui_surface_clear(s, COL_BG);
    gui_window_draw_decorations(win);

    y = GUI_TITLEBAR_HEIGHT + PAD;

    /* Title */
    {
        const char* title = "System Updates";
        int tw = str_length(title) * THEME_TYPO_TITLE_W;
        gui_surface_draw_string_2x(s, (w - tw) / 2, y, title, COL_ACCENT, 0, 0);
        y += THEME_TYPO_TITLE_H + 8;
    }

    /* Current version card */
    upd_draw_card(s, PAD, y, w - 2 * PAD, 56, COL_PANEL, COL_BORDER);
    gui_surface_draw_string(s, PAD + 12, y + 8, "Version actual", COL_LABEL, 0, 0);
    gui_surface_draw_string_2x(s, PAD + 12, y + 24, LYTH_KERNEL_PRETTY_NAME,
                               COL_TEXT, 0, 0);
    y += 56 + 10;

    /* Status + button card */
    upd_draw_card(s, PAD, y, w - 2 * PAD, 76, COL_PANEL, COL_BORDER);
    {
        int cx = PAD + 12;
        int cy = y + 10;
        const char* st_text;
        uint32_t st_col;

        switch (upd_state) {
        case ST_IDLE:        st_text = "No comprobado";                     st_col = COL_DIM;    break;
        case ST_CHECKING:    st_text = "Comprobando actualizaciones...";    st_col = COL_YELLOW; break;
        case ST_UP_TO_DATE:  st_text = "El sistema esta actualizado";       st_col = COL_GREEN;  break;
        case ST_DOWNLOADING: st_text = "Descargando actualizacion...";      st_col = COL_YELLOW; break;
        case ST_INSTALLED:   st_text = "Actualizado (reinicia para aplicar)"; st_col = COL_GREEN; break;
        default:             st_text = "Desconocido";                       st_col = COL_DIM;    break;
        }

        gui_surface_draw_string(s, cx, cy, "Estado:", COL_LABEL, 0, 0);
        gui_surface_draw_string(s, cx + 8 * FONT_PSF_WIDTH, cy, st_text, st_col, 0, 0);
        cy += GUI_FONT_H + 6;

        if (upd_state == ST_CHECKING || upd_state == ST_DOWNLOADING) {
            int pb_w = w - 2 * PAD - 24;
            int pb_h = 12;
            gui_surface_fill(s, cx, cy, pb_w, pb_h, COL_BORDER);
            int fill_w = (pb_w * upd_progress) / 100;
            if (fill_w > 0) gui_surface_fill(s, cx, cy, fill_w, pb_h, COL_ACCENT);
            char pct[8];
            uint_to_str((unsigned int)upd_progress, pct, sizeof(pct));
            str_append(pct, "%", sizeof(pct));
            int pw = str_length(pct) * FONT_PSF_WIDTH;
            gui_surface_draw_string(s, cx + (pb_w - pw) / 2, cy + 1, pct, COL_TEXT, 0, 0);
        } else {
            int bw = 200;
            int bh = 22;
            const char* blbl = "Comprobar actualizaciones";
            if (upd_state == ST_UP_TO_DATE || upd_state == ST_INSTALLED)
                blbl = "Comprobar de nuevo";
            gui_surface_fill(s, cx, cy, bw, bh, COL_ACCENT);
            int blw = str_length(blbl) * FONT_PSF_WIDTH;
            if (blw > bw - 4) blw = bw - 4;
            gui_surface_draw_string(s, cx + (bw - blw) / 2, cy + 4, blbl, 0xFFFFFFFF, 0, 0);
        }
    }
    y += 76 + 12;

    /* Separator */
    gui_surface_hline(s, PAD, y, w - 2 * PAD, COL_BORDER);
    y += 12;

    /* Changelog */
    gui_surface_draw_string_2x(s, PAD, y, "Historial de cambios", COL_ACCENT, 0, 0);
    y += THEME_TYPO_SUBTITLE_H + 8;

    {
        int i;
        for (i = 0; i < CHANGELOG_COUNT; i++) {
            if (y + GUI_FONT_H + 4 >= (int)win->height - 8) break;
            char header[48];
            header[0] = '\0';
            str_append(header, "v", sizeof(header));
            str_append(header, changelog[i].version, sizeof(header));
            str_append(header, "  (", sizeof(header));
            str_append(header, changelog[i].date, sizeof(header));
            str_append(header, ")", sizeof(header));
            gui_surface_draw_string(s, PAD + 4, y, header, COL_ACCENT, 0, 0);
            y += GUI_FONT_H + 2;
            if (y + GUI_FONT_H + 4 < (int)win->height - 8) {
                gui_surface_draw_string(s, PAD + 16, y, changelog[i].summary, COL_DIM, 0, 0);
                y += GUI_FONT_H + 6;
            }
        }
    }
}

/* ---- Callbacks ---- */

static void upd_on_close(gui_window_t* win) {
    upd_is_open = 0;
    upd_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

static void upd_on_key(gui_window_t* win, int event_type, char key) {
    if (event_type == INPUT_EVENT_CHAR && key == 27)
        gui_window_close_animated(upd_window);
    (void)win;
}

static void upd_on_click(gui_window_t* win, int mx, int my, int button) {
    (void)button;
    int card_y = GUI_TITLEBAR_HEIGHT + PAD + THEME_TYPO_TITLE_H + 8 + 56 + 10;
    int cx = PAD + 12;
    int cy = card_y + 10 + GUI_FONT_H + 6;
    int bw = 200;
    int bh = 22;

    if (upd_state == ST_IDLE || upd_state == ST_UP_TO_DATE ||
        upd_state == ST_INSTALLED) {
        if (mx >= cx && mx < cx + bw && my >= cy && my < cy + bh) {
            upd_state = ST_CHECKING;
            upd_progress = 0;
            upd_check_start = timer_get_ticks();
            audio_play_sound(SND_CLICK);
            gui_window_invalidate(win);
        }
    }
}

/* ---- Public API ---- */

void updater_app_open(void) {
    if (upd_is_open && upd_window) {
        gui_window_focus(upd_window);
        gui_window_invalidate(upd_window);
        return;
    }

    upd_state = ST_IDLE;
    upd_progress = 0;

    upd_window = gui_window_create("Updates", 200, 60, UPD_WIN_W, UPD_WIN_H,
                                    GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE |
                                    GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!upd_window) return;

    upd_window->on_paint = upd_paint;
    upd_window->on_close = upd_on_close;
    upd_window->on_key   = upd_on_key;
    upd_window->on_click = upd_on_click;
    upd_is_open = 1;

    gui_window_invalidate(upd_window);
}
