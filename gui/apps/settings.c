/*
 * Settings app — system configuration panel with sidebar navigation.
 *
 * Sections: Apariencia, Fondo, Pantalla, Sonido, Red, Sistema, Usuarios,
 *           Acerca de.
 *
 * Resolution change through VBE is not available at runtime with the
 * current multiboot framebuffer backend, so we show info + debug overlay.
 */

#include "settings.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "fbconsole.h"
#include "video.h"
#include "session.h"
#include "desktop.h"

/* ---- colours ---- */
#define COL_BG          THEME_COL_BASE
#define COL_SIDEBAR     THEME_COL_MANTLE
#define COL_TEXT        THEME_COL_TEXT
#define COL_DIM         THEME_COL_DIM
#define COL_ACCENT      THEME_COL_ACCENT
#define COL_LABEL       THEME_COL_SUBTEXT0
#define COL_GOOD        THEME_COL_SUCCESS
#define COL_WARN        THEME_COL_WARNING
#define COL_SEP         THEME_COL_BORDER
#define COL_HOVER       THEME_COL_SURFACE0
#define COL_CARD        THEME_COL_SURFACE0
#define COL_CARD_BRD    THEME_COL_BORDER_DIM
#define COL_BTN         THEME_COL_ACCENT
#define COL_BTN_SEC     THEME_COL_SURFACE1

/* ---- layout ---- */
#define SIDEBAR_W       140
#define ITEM_H          28
#define WIN_W           640
#define WIN_H           580

/* ---- sections ---- */
#define SEC_APARIENCIA  0
#define SEC_FONDO       1
#define SEC_PANTALLA    2
#define SEC_SONIDO      3
#define SEC_RED         4
#define SEC_SISTEMA     5
#define SEC_USUARIOS    6
#define SEC_ACERCA      7
#define SEC_COUNT       8

static const char* sec_names[SEC_COUNT] = {
    "Apariencia", "Fondo", "Pantalla", "Sonido",
    "Red", "Sistema", "Usuarios", "Acerca de"
};

static gui_window_t* set_window;
static int set_open;
static int set_section = SEC_FONDO;

/* Fondo page state */
static int fondo_sel    = -1;  /* -1 = use desktop current */
static int fondo_blur   = 0;
static int fondo_scale  = 0;   /* 0=Cubrir, 1=Ajustar, 2=Estirar */
static const char* scale_labels[] = { "Cubrir", "Ajustar", "Estirar" };
#define SCALE_COUNT 3

/* Thumbnail layout constants (content-relative coordinates) */
#define THUMB_W     62
#define THUMB_H     42
#define THUMB_GAP   8
#define THUMB_OX    0   /* relative to content ox */

/* ---- helpers ---- */
static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

/* Draw a simple outlined button label */
static void draw_button(gui_surface_t* s, int x, int y, int w, int h,
                        const char* label, uint32_t bg, uint32_t fg) {
    gui_surface_fill(s, x, y, w, h, bg);
    /* Border */
    gui_surface_hline(s, x, y, w, COL_CARD_BRD);
    gui_surface_hline(s, x, y + h - 1, w, COL_CARD_BRD);
    gui_surface_fill(s, x, y, 1, h, COL_CARD_BRD);
    gui_surface_fill(s, x + w - 1, y, 1, h, COL_CARD_BRD);
    /* Label centered */
    {
        int tw = (int)strlen(label) * FONT_PSF_WIDTH;
        int tx = x + (w - tw) / 2;
        int ty = y + (h - FONT_PSF_HEIGHT) / 2;
        gui_surface_draw_string(s, tx, ty, label, fg, 0, 0);
    }
}

/* Draw a card (rounded-ish rect with border) */
static void draw_card(gui_surface_t* s, int x, int y, int w, int h) {
    gui_surface_fill(s, x, y, w, h, COL_CARD);
    gui_surface_hline(s, x, y, w, COL_CARD_BRD);
    gui_surface_hline(s, x, y + h - 1, w, COL_CARD_BRD);
    gui_surface_fill(s, x, y, 1, h, COL_CARD_BRD);
    gui_surface_fill(s, x + w - 1, y, 1, h, COL_CARD_BRD);
}

/* Draw a toggle switch (on/off) */
static void draw_toggle(gui_surface_t* s, int x, int y, int on) {
    int tw = 36, th = 18;
    uint32_t bg = on ? COL_ACCENT : THEME_COL_SURFACE2;
    gui_surface_fill(s, x, y, tw, th, bg);
    gui_surface_hline(s, x, y, tw, COL_CARD_BRD);
    gui_surface_hline(s, x, y + th - 1, tw, COL_CARD_BRD);
    gui_surface_fill(s, x, y, 1, th, COL_CARD_BRD);
    gui_surface_fill(s, x + tw - 1, y, 1, th, COL_CARD_BRD);
    /* Knob */
    {
        int kx = on ? x + tw - 16 : x + 2;
        gui_surface_fill(s, kx, y + 2, 14, 14, 0xFFFFFF);
    }
}

/* Draw a slider bar */
static void draw_slider(gui_surface_t* s, int x, int y, int w, int pct) {
    int bar_h = 4;
    int track_y = y + 7;
    int filled = w * pct / 100;
    /* Track bg */
    gui_surface_fill(s, x, track_y, w, bar_h, THEME_COL_SURFACE2);
    /* Filled part */
    if (filled > 0)
        gui_surface_fill(s, x, track_y, filled, bar_h, COL_ACCENT);
    /* Knob */
    {
        int kx = x + filled - 6;
        if (kx < x) kx = x;
        gui_surface_fill(s, kx, y + 2, 12, 14, COL_ACCENT);
        gui_surface_fill(s, kx + 1, y + 3, 10, 12, 0xE0E0E0);
    }
}

/* ---- sidebar ---- */
static void draw_sidebar(gui_surface_t* s, int win_h) {
    int i;
    int oy = GUI_TITLEBAR_HEIGHT;
    int item_start = oy + 8;

    gui_surface_fill(s, 0, oy, SIDEBAR_W, win_h - oy, COL_SIDEBAR);
    gui_surface_fill(s, SIDEBAR_W - 1, oy, 1, win_h - oy, COL_SEP);

    for (i = 0; i < SEC_COUNT; i++) {
        int iy = item_start + i * ITEM_H;

        if (i == set_section) {
            gui_surface_fill(s, 0, iy, SIDEBAR_W - 1, ITEM_H, COL_HOVER);
            gui_surface_fill(s, 0, iy + 4, 3, ITEM_H - 8, COL_ACCENT);
        }

        gui_surface_draw_string(s, 16, iy + (ITEM_H - FONT_PSF_HEIGHT) / 2,
            sec_names[i],
            (i == set_section) ? COL_ACCENT : COL_LABEL, 0, 0);
    }

    /* Username at bottom */
    {
        const session_t* sess = session_get_current();
        if (sess && sess->active && sess->username[0]) {
            gui_surface_draw_string(s, 12, win_h - FONT_PSF_HEIGHT - 10,
                sess->username, COL_DIM, 0, 0);
        }
    }
}

/* ---- page: Apariencia ---- */
static void page_apariencia(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    gui_surface_draw_string(s, ox, oy, "Apariencia", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy, "Personaliza el aspecto visual.", COL_DIM, 0, 0);
    oy += rh + 8;

    gui_surface_draw_string(s, ox, oy, "Tema:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, "Catppuccin Mocha", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Acento:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, "Cyan (#00D4FF)", COL_ACCENT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Fuente:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, "Inter 8x16 PSF", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Barra titulo:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, "macOS (traffic lights)", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Dock:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 120, oy, "Centro pill translucido", COL_TEXT, 0, 0);
    oy += rh + 8;

    /* Palette swatches */
    gui_surface_draw_string(s, ox, oy, "Paleta de colores", COL_LABEL, 0, 0);
    oy += rh;
    {
        static const uint32_t sw[] = {
            THEME_COL_ACCENT, THEME_COL_SUCCESS, THEME_COL_WARNING,
            THEME_COL_ERROR, THEME_COL_INFO, THEME_COL_ACCENT_PINK,
            THEME_COL_ACCENT_ALT, THEME_COL_FOCUS
        };
        int si;
        for (si = 0; si < 8; si++)
            gui_surface_fill(s, ox + si * 30, oy, 24, 16, sw[si]);
    }
    oy += 28;

    gui_surface_draw_string(s, ox, oy,
        "Editar tema: include/gui/theme.h", COL_DIM, 0, 0);
}

/* ---- page: Fondo ---- */
/* Y positions stored during paint for click handling (surface-relative) */
static int fondo_thumb_sy;    /* surface Y of thumbnail row */
static int fondo_apply_sy;    /* surface Y of Apply/Predeterminado buttons */
static int fondo_toggle_sy;   /* surface Y of blur toggle */
static int fondo_scale_sy;    /* surface Y of scale mode dropdown */

/* Nearest-neighbor cover blit: scale src image to fill dst rect, crop excess */
static void nn_cover_blit(gui_surface_t *dst, int dx, int dy, int dw, int dh,
                           const uint32_t *src, int iw, int ih)
{
    int scale_num, scale_den, sx_off = 0, sy_off = 0, y;
    if ((long)iw * dh > (long)ih * dw) {
        scale_num = dh; scale_den = ih;
        sx_off = ((int)((long)iw * dh / ih) - dw) / 2;
    } else {
        scale_num = dw; scale_den = iw;
        sy_off = ((int)((long)ih * dw / iw) - dh) / 2;
    }
    for (y = 0; y < dh; y++) {
        int sy = (int)((long)(y + sy_off) * scale_den / scale_num);
        int x;
        if (sy < 0) sy = 0; if (sy >= ih) sy = ih - 1;
        if (dy + y < 0 || dy + y >= dst->height) continue;
        for (x = 0; x < dw; x++) {
            int sxi = (int)((long)(x + sx_off) * scale_den / scale_num);
            if (sxi < 0) sxi = 0; if (sxi >= iw) sxi = iw - 1;
            if (dx + x >= 0 && dx + x < dst->width)
                dst->pixels[(dy + y) * dst->width + (dx + x)] = src[sy * iw + sxi];
        }
    }
}

static void page_fondo(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    int pw, ph, wcount, cur_sel, ti;
    int wp_iw = 0, wp_ih = 0;
    const uint32_t *wp_px = 0;

    cur_sel = (fondo_sel < 0) ? desktop_wallpaper_selected() : fondo_sel;
    wcount = desktop_wallpaper_count();

    gui_surface_draw_string(s, ox, oy, "Fondo", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Personaliza la imagen del escritorio.", COL_DIM, 0, 0);
    oy += rh + 8;

    /* Vista previa */
    gui_surface_draw_string(s, ox, oy, "Vista previa", COL_LABEL, 0, 0);
    oy += rh;
    pw = cw - 16;
    if (pw > 360) pw = 360;
    ph = pw * 9 / 16;
    draw_card(s, ox, oy, pw, ph);
    {
        if (cur_sel >= 0 && cur_sel < wcount) {
            if (desktop_wallpaper_is_image(cur_sel)) {
                /* Try to get decoded pixels for this wallpaper */
                wp_px = desktop_wallpaper_pixels(cur_sel, &wp_iw, &wp_ih);
                if (wp_px) {
                    nn_cover_blit(s, ox + 1, oy + 1, pw - 2, ph - 2,
                                  wp_px, wp_iw, wp_ih);
                } else {
                    /* Not yet loaded — show placeholder with name */
                    gui_surface_fill(s, ox + 1, oy + 1, pw - 2, ph - 2,
                                     THEME_COL_SURFACE0);
                    gui_surface_draw_string(s, ox + pw / 2 - 24,
                        oy + ph / 2 - 8,
                        desktop_wallpaper_name(cur_sel), COL_DIM, 0, 0);
                }
            } else {
                gui_surface_fill(s, ox + 1, oy + 1, pw - 2, ph - 2,
                                 desktop_wallpaper_solid_col(cur_sel));
            }
        } else {
            gui_surface_fill(s, ox + 1, oy + 1, pw - 2, ph - 2,
                             THEME_COL_CRUST);
        }
    }
    oy += ph + 10;

    /* Fondos de pantalla section */
    gui_surface_draw_string(s, ox, oy, "Fondos de pantalla", COL_LABEL, 0, 0);
    oy += rh;

    /* Store Y for click handling */
    fondo_thumb_sy = oy;

    /* Thumbnail grid */
    for (ti = 0; ti < wcount && ti < 8; ti++) {
        int tx = ox + ti * (THUMB_W + THUMB_GAP);

        draw_card(s, tx, oy, THUMB_W, THUMB_H);

        if (desktop_wallpaper_is_image(ti)) {
            int tiw = 0, tih = 0;
            const uint32_t *tpx = desktop_wallpaper_pixels(ti, &tiw, &tih);
            if (tpx) {
                /* Real image thumbnail */
                nn_cover_blit(s, tx + 1, oy + 1, THUMB_W - 2, THUMB_H - 2,
                              tpx, tiw, tih);
            } else {
                /* Not loaded — styled placeholder */
                gui_surface_fill(s, tx + 1, oy + 1, THUMB_W - 2, THUMB_H - 2,
                                 THEME_COL_SURFACE0);
                gui_surface_draw_string(s, tx + 4, oy + THUMB_H / 2 - 8,
                    desktop_wallpaper_name(ti), COL_DIM, 0, 0);
            }
        } else {
            gui_surface_fill(s, tx + 1, oy + 1, THUMB_W - 2, THUMB_H - 2,
                             desktop_wallpaper_solid_col(ti));
        }

        /* Selection border */
        if (ti == cur_sel) {
            gui_surface_hline(s, tx, oy, THUMB_W, COL_ACCENT);
            gui_surface_hline(s, tx, oy + THUMB_H - 1, THUMB_W, COL_ACCENT);
            gui_surface_fill(s, tx, oy, 1, THUMB_H, COL_ACCENT);
            gui_surface_fill(s, tx + THUMB_W - 1, oy, 1, THUMB_H, COL_ACCENT);
            gui_surface_hline(s, tx + 1, oy + 1, THUMB_W - 2, COL_ACCENT);
            gui_surface_fill(s, tx + 1, oy + 1, 1, THUMB_H - 2, COL_ACCENT);
            gui_surface_fill(s, tx + THUMB_W - 2, oy + 1, 1, THUMB_H - 2, COL_ACCENT);
        }
    }
    oy += THUMB_H + 10;

    /* Scale mode */
    fondo_scale_sy = oy;
    gui_surface_draw_string(s, ox, oy, "Ajuste de imagen:", COL_LABEL, 0, 0);
    draw_card(s, ox + 150, oy - 2, 100, FONT_PSF_HEIGHT + 6);
    gui_surface_draw_string(s, ox + 158, oy, scale_labels[fondo_scale], COL_TEXT, 0, 0);
    oy += rh + 4;

    /* Blur toggle */
    fondo_toggle_sy = oy;
    gui_surface_draw_string(s, ox, oy, "Blur del fondo:", COL_LABEL, 0, 0);
    draw_toggle(s, ox + 150, oy - 1, fondo_blur);
    gui_surface_draw_string(s, ox + 192, oy,
        fondo_blur ? "Activado" : "Desactivado",
        fondo_blur ? COL_ACCENT : COL_DIM, 0, 0);
    oy += rh + 12;

    /* Apply / Reset buttons */
    fondo_apply_sy = oy;
    draw_button(s, ox, oy, 100, 28, "Aplicar", COL_BTN, 0x000000);
    draw_button(s, ox + 112, oy, 140, 28, "Predeterminado", COL_BTN_SEC, COL_TEXT);
}

/* ---- page: Pantalla ---- */
static void page_pantalla(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    char buf[64];

    gui_surface_draw_string(s, ox, oy, "Pantalla", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Informacion de la pantalla actual.", COL_DIM, 0, 0);
    oy += rh + 10;

    /* Resolution */
    {
        int pos = 0;
        int_to_str(fb_width(), buf, sizeof(buf)); pos = (int)strlen(buf);
        buf[pos++] = ' '; buf[pos++] = 'x'; buf[pos++] = ' ';
        int_to_str(fb_height(), buf + pos, sizeof(buf) - pos);
    }
    gui_surface_draw_string(s, ox, oy, "Resolucion:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    /* BPP */
    int_to_str(fb_bpp(), buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " bpp", 5); }
    gui_surface_draw_string(s, ox, oy, "Profundidad:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    /* Backend */
    gui_surface_draw_string(s, ox, oy, "Backend:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, video_backend_name(), COL_TEXT, 0, 0);
    oy += rh;

    /* Pitch */
    int_to_str(fb_pitch(), buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " B/fila", 8); }
    gui_surface_draw_string(s, ox, oy, "Pitch:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    /* Refresh */
    gui_surface_draw_string(s, ox, oy, "Refresco:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "60 Hz (vsync)", COL_TEXT, 0, 0);
    oy += rh + 12;

    gui_surface_draw_string(s, ox, oy,
        "Cambio de resolucion no disponible en", COL_DIM, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "el backend multiboot actual.", COL_DIM, 0, 0);
}

/* ---- page: Sonido ---- */
static void page_sonido(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    gui_surface_draw_string(s, ox, oy, "Sonido", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Configuracion de audio.", COL_DIM, 0, 0);
    oy += rh + 10;

    gui_surface_draw_string(s, ox, oy,
        "Sin dispositivos de audio detectados.", COL_LABEL, 0, 0);
    oy += rh + 4;
    gui_surface_draw_string(s, ox, oy,
        "El driver de audio no esta implementado", COL_DIM, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "en esta version del kernel.", COL_DIM, 0, 0);
}

/* ---- page: Red ---- */
static void page_red(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    gui_surface_draw_string(s, ox, oy, "Red", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Configuracion de red.", COL_DIM, 0, 0);
    oy += rh + 10;

    gui_surface_draw_string(s, ox, oy, "Interfaz:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "e1000 (Intel PRO/1000)", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Estado:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "Conectado", COL_GOOD, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Protocolo:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "DHCP", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Servicios:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "ARP, ICMP, TCP, UDP, DNS", COL_TEXT, 0, 0);
}

/* ---- page: Sistema (perf metrics) ---- */
static void page_sistema(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    char buf[64];
    gui_metrics_t met;

    gui_surface_draw_string(s, ox, oy, "Sistema", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Rendimiento y metricas del compositor.", COL_DIM, 0, 0);
    oy += rh + 10;

    gui_get_metrics(&met);

    int_to_str(met.fps, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " /s", 4); }
    gui_surface_draw_string(s, ox, oy, "Frames:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf,
                            met.fps >= 50 ? COL_GOOD : COL_WARN, 0, 0);
    oy += rh;

    int_to_str(met.frame_time_avg, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox, oy, "Frame prom.:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    int_to_str(met.frame_time_max, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox, oy, "Frame max:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf,
                            met.frame_time_max > 16000 ? COL_WARN : COL_TEXT, 0, 0);
    oy += rh;

    int_to_str(met.compose_us, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox, oy, "Compose:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    int_to_str(met.present_us, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " us", 4); }
    gui_surface_draw_string(s, ox, oy, "Present:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    int_to_str(met.dirty_count, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " rects", 7); }
    gui_surface_draw_string(s, ox, oy, "Dirty rects:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    int_to_str(met.pixels_copied, buf, sizeof(buf));
    { int p = (int)strlen(buf); memcpy(buf + p, " px", 4); }
    gui_surface_draw_string(s, ox, oy, "Pixeles:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, buf, COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Arrastrando:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy,
                            met.drag_active ? "SI" : "no",
                            met.drag_active ? COL_WARN : COL_DIM, 0, 0);
    oy += rh + 8;

    gui_surface_draw_string(s, ox, oy,
        "Pulsa R para actualizar metricas.", COL_DIM, 0, 0);
}

/* ---- page: Usuarios ---- */
static void page_usuarios(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    gui_surface_draw_string(s, ox, oy, "Usuarios", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Cuentas de usuario del sistema.", COL_DIM, 0, 0);
    oy += rh + 10;

    {
        const session_t* sess = session_get_current();
        gui_surface_draw_string(s, ox, oy, "Usuario:", COL_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 140, oy,
            (sess && sess->username[0]) ? sess->username : "root",
            COL_TEXT, 0, 0);
        oy += rh;

        gui_surface_draw_string(s, ox, oy, "UID:", COL_LABEL, 0, 0);
        if (sess) {
            char ubuf[12];
            int_to_str(sess->uid, ubuf, sizeof(ubuf));
            gui_surface_draw_string(s, ox + 140, oy, ubuf, COL_TEXT, 0, 0);
        }
        oy += rh;

        gui_surface_draw_string(s, ox, oy, "Sesion:", COL_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 140, oy,
            (sess && sess->active) ? "Activa" : "Inactiva",
            (sess && sess->active) ? COL_GOOD : COL_DIM, 0, 0);
        oy += rh;

        gui_surface_draw_string(s, ox, oy, "Home:", COL_LABEL, 0, 0);
        gui_surface_draw_string(s, ox + 140, oy,
            (sess && sess->home[0]) ? sess->home : "/",
            COL_TEXT, 0, 0);
    }
}

/* ---- page: Acerca de ---- */
static void page_acerca(gui_surface_t* s, int ox, int oy, int cw, int rh) {
    gui_surface_draw_string(s, ox, oy, "Acerca de Lyth", COL_TEXT, 0, 0);
    oy += rh;
    gui_surface_draw_string(s, ox, oy,
        "Informacion del sistema operativo.", COL_DIM, 0, 0);
    oy += rh + 10;

    gui_surface_draw_string(s, ox, oy, "Nombre:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "Lyth OS", COL_ACCENT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Arquitectura:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "x86_64", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Bootloader:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "GRUB2 Multiboot", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Compilador:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "GCC (freestanding)", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "GUI:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "Compositor dirty-rect 60Hz", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Fuente:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "Inter Regular PSF2", COL_TEXT, 0, 0);
    oy += rh;

    gui_surface_draw_string(s, ox, oy, "Tema:", COL_LABEL, 0, 0);
    gui_surface_draw_string(s, ox + 140, oy, "Catppuccin Mocha", COL_TEXT, 0, 0);
    oy += rh + 10;

    gui_surface_draw_string(s, ox, oy, "Hecho con", COL_DIM, 0, 0);
    gui_surface_draw_string(s, ox + 80, oy, "<3", THEME_COL_ERROR, 0, 0);
    gui_surface_draw_string(s, ox + 100, oy, "por Adrian Torres Graña", COL_DIM, 0, 0);
}

/* ---- paint ---- */
static void set_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int ox = SIDEBAR_W + 20;
    int oy = GUI_TITLEBAR_HEIGHT + 16;
    int cw = win->width - SIDEBAR_W - 32;
    int rh = FONT_PSF_HEIGHT + 6;

    if (!s->pixels) return;

    gui_surface_clear(s, COL_BG);
    gui_window_draw_decorations(win);
    draw_sidebar(s, win->height);

    switch (set_section) {
        case SEC_APARIENCIA: page_apariencia(s, ox, oy, cw, rh); break;
        case SEC_FONDO:      page_fondo(s, ox, oy, cw, rh);      break;
        case SEC_PANTALLA:   page_pantalla(s, ox, oy, cw, rh);   break;
        case SEC_SONIDO:     page_sonido(s, ox, oy, cw, rh);     break;
        case SEC_RED:        page_red(s, ox, oy, cw, rh);        break;
        case SEC_SISTEMA:    page_sistema(s, ox, oy, cw, rh);    break;
        case SEC_USUARIOS:   page_usuarios(s, ox, oy, cw, rh);   break;
        case SEC_ACERCA:     page_acerca(s, ox, oy, cw, rh);     break;
    }
}

/* ---- input ---- */
static void set_on_key(gui_window_t* win, int event_type, char key) {
    (void)event_type;
    if (key == 'r' || key == 'R') {
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
    if (key >= '1' && key <= '0' + SEC_COUNT) {
        set_section = key - '1';
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

static void fondo_handle_click(gui_window_t* win, int sx, int sy) {
    int content_ox = SIDEBAR_W + 20;
    int wcount = desktop_wallpaper_count();
    int i, tx;

    /* Thumbnail click */
    if (sy >= fondo_thumb_sy && sy < fondo_thumb_sy + THUMB_H) {
        for (i = 0; i < wcount && i < 8; i++) {
            tx = content_ox + i * (THUMB_W + THUMB_GAP);
            if (sx >= tx && sx < tx + THUMB_W) {
                fondo_sel = i;
                win->needs_redraw = 1;
                gui_dirty_add(win->x, win->y, win->width, win->height);
                return;
            }
        }
    }

    /* Scale mode click */
    if (sy >= fondo_scale_sy - 2 && sy < fondo_scale_sy + FONT_PSF_HEIGHT + 6) {
        if (sx >= content_ox + 150 && sx < content_ox + 250) {
            fondo_scale = (fondo_scale + 1) % SCALE_COUNT;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
    }

    /* Blur toggle click */
    if (sy >= fondo_toggle_sy - 2 && sy < fondo_toggle_sy + FONT_PSF_HEIGHT + 4) {
        if (sx >= content_ox + 150 && sx < content_ox + 192) {
            fondo_blur = !fondo_blur;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
    }

    /* Apply button */
    if (sy >= fondo_apply_sy && sy < fondo_apply_sy + 28) {
        if (sx >= content_ox && sx < content_ox + 100) {
            int sel = (fondo_sel < 0) ? desktop_wallpaper_selected() : fondo_sel;
            desktop_set_wallpaper(sel);
            fondo_sel = -1;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
        /* Predeterminado button */
        if (sx >= content_ox + 112 && sx < content_ox + 252) {
            desktop_set_wallpaper(0);
            fondo_sel = -1;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
    }
}

static void set_on_click(gui_window_t* win, int lx, int ly, int button) {
    (void)button;
    int item_h = ITEM_H;
    /*
     * lx, ly are content-area relative (titlebar + border already subtracted
     * by the compositor).  Sidebar items are drawn at surface y =
     * GUI_TITLEBAR_HEIGHT + 8 + i * ITEM_H, which in content-area coords
     * equals (8 - GUI_BORDER_WIDTH + i * ITEM_H).
     */
    int items_start = 8 - GUI_BORDER_WIDTH;
    int sx = lx + GUI_BORDER_WIDTH; /* surface x */
    int sy = ly + GUI_TITLEBAR_HEIGHT; /* surface y */

    /* Sidebar */
    if (sx < SIDEBAR_W && ly >= items_start) {
        int idx = (ly - items_start) / item_h;
        if (idx >= 0 && idx < SEC_COUNT) {
            set_section = idx;
            win->needs_redraw = 1;
            gui_dirty_add(win->x, win->y, win->width, win->height);
            return;
        }
    }

    /* Content area — dispatch per page */
    if (sx >= SIDEBAR_W) {
        if (set_section == SEC_FONDO) {
            fondo_handle_click(win, sx, sy);
        }
    }
}

static void set_on_close(gui_window_t* win) {
    set_open = 0;
    set_window = 0;
    gui_dirty_add(win->x - 8, win->y - 8, win->width + 16, win->height + 16);
    gui_window_destroy(win);
}

void settings_app_open(void) {
    if (set_open && set_window) {
        gui_window_focus(set_window);
        gui_dirty_add(set_window->x, set_window->y,
                      set_window->width, set_window->height);
        return;
    }

    set_window = gui_window_create("Settings", 180, 40, WIN_W, WIN_H,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!set_window) return;

    set_section = SEC_FONDO;
    set_window->on_paint = set_paint;
    set_window->on_key = set_on_key;
    set_window->on_click = set_on_click;
    set_window->on_close = set_on_close;

    set_open = 1;
    gui_window_focus(set_window);
    gui_dirty_add(set_window->x, set_window->y,
                  set_window->width, set_window->height);
}
