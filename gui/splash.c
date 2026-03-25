/* ============================================================
 *  splash.c  —  Boot splash screen
 *
 *  Renders directly to the framebuffer without the compositor.
 *  Shows the Lyth OS logo, a short status message and a simple
 *  progress bar during early boot.
 * ============================================================ */

#include "splash.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "string.h"
#include "theme.h"

/* ---- Colours (from theme.h) ---- */
#define COL_BG        THEME_COL_BASE
#define COL_LOGO      THEME_COL_TEXT
#define COL_MSG       THEME_COL_SUBTEXT0
#define COL_BAR_BG    THEME_COL_SURFACE0
#define COL_BAR_FG    THEME_COL_ACCENT

/* ---- Layout ---- */
#define LOGO_TEXT     "Lyth OS"
#define BAR_WIDTH     300
#define BAR_HEIGHT    8

/* ---- State ---- */
static int splash_visible;
static int splash_progress;
static char splash_msg[80];

/* ---- Low-level framebuffer pixel writers ---- */

static void splash_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    uint8_t* fb = (uint8_t*)fb_get_buffer();
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();
    uint32_t pitch = fb_pitch();
    int bpp = (int)fb_bpp();
    int row, col;

    if (!fb || bpp != 32) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;

    for (row = y; row < y + h; row++) {
        uint32_t* line = (uint32_t*)(fb + row * pitch);
        for (col = x; col < x + w; col++) {
            line[col] = rgb;
        }
    }
}

static void splash_draw_char(int x, int y, unsigned char ch, uint32_t fg) {
    uint8_t* fb = (uint8_t*)fb_get_buffer();
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();
    uint32_t pitch = fb_pitch();
    int row, col;
    uint8_t bits;

    if (!fb || fb_bpp() != 32) return;

    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= scr_h) continue;
        uint32_t* line = (uint32_t*)(fb + py * pitch);
        bits = font_psf_data[ch][row];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= scr_w) continue;
            if (bits & (0x80 >> col)) {
                line[px] = fg;
            }
        }
    }
}

static void splash_draw_string(int x, int y, const char* str, uint32_t fg) {
    while (*str) {
        splash_draw_char(x, y, (unsigned char)*str, fg);
        x += FONT_PSF_WIDTH;
        str++;
    }
}

static int splash_string_width(const char* str) {
    int len = 0;
    while (str[len]) len++;
    return len * FONT_PSF_WIDTH;
}

/* ---- Redraw ---- */

/* Cached layout positions so we can clear only what changes */
static int splash_logo_x, splash_logo_y;
static int splash_msg_y;
static int splash_bar_x, splash_bar_y;

static void splash_redraw_full(void) {
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();

    /* Full background — only called once during splash_show */
    splash_fill_rect(0, 0, scr_w, scr_h, COL_BG);

    /* Cache layout positions */
    splash_logo_x = (scr_w - splash_string_width(LOGO_TEXT)) / 2;
    splash_logo_y = (scr_h * 2) / 5 - FONT_PSF_HEIGHT;
    splash_msg_y  = splash_logo_y + FONT_PSF_HEIGHT + FONT_PSF_HEIGHT;
    splash_bar_x  = (scr_w - BAR_WIDTH) / 2;
    splash_bar_y  = splash_logo_y + FONT_PSF_HEIGHT * 4;

    /* Logo (static, drawn once) */
    splash_draw_string(splash_logo_x, splash_logo_y, LOGO_TEXT, COL_LOGO);
}

static void splash_redraw(void) {
    int scr_w = (int)fb_width();

    /* Clear only the message area, then redraw text */
    splash_fill_rect(0, splash_msg_y, scr_w, FONT_PSF_HEIGHT, COL_BG);
    if (splash_msg[0]) {
        int mx = (scr_w - splash_string_width(splash_msg)) / 2;
        splash_draw_string(mx, splash_msg_y, splash_msg, COL_MSG);
    }

    /* Redraw progress bar (small fixed-size area) */
    splash_fill_rect(splash_bar_x, splash_bar_y, BAR_WIDTH, BAR_HEIGHT, COL_BAR_BG);
    {
        int fill_w = (BAR_WIDTH * splash_progress) / 100;
        if (fill_w < 0) fill_w = 0;
        if (fill_w > BAR_WIDTH) fill_w = BAR_WIDTH;
        if (fill_w > 0) {
            splash_fill_rect(splash_bar_x, splash_bar_y, fill_w, BAR_HEIGHT, COL_BAR_FG);
        }
    }
}

/* ---- Public API ---- */

void splash_show(void) {
    if (!fb_active() || fb_bpp() != 32) return;
    splash_visible = 1;
    splash_progress = 0;
    splash_msg[0] = '\0';
    splash_redraw_full();
}

void splash_set_message(const char* msg) {
    if (!splash_visible) return;
    if (msg) {
        int i = 0;
        while (msg[i] && i + 1 < (int)sizeof(splash_msg)) {
            splash_msg[i] = msg[i];
            i++;
        }
        splash_msg[i] = '\0';
    } else {
        splash_msg[0] = '\0';
    }
    splash_redraw();
}

void splash_set_progress(int percent) {
    if (!splash_visible) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    splash_progress = percent;
    splash_redraw();
}

void splash_hide(void) {
    if (!splash_visible) return;
    splash_visible = 0;
    fb_clear();
}
