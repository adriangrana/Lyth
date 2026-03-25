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
#include "lib/png.h"
#include "physmem.h"
#include "splash_png.h"

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
static png_image_t splash_img;
static int splash_img_loaded;

/* Cached scaling params from splash_redraw_full */
static int splash_scale_num, splash_scale_den;
static int splash_sx_off, splash_sy_off;

/* ---- Bilinear interpolation helper ---- */
static uint32_t bilinear_sample(const uint32_t *pixels, int iw, int ih,
                                int fx16, int fy16)
{
    int x0 = fx16 >> 16;
    int y0 = fy16 >> 16;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    int fx = fx16 & 0xFFFF;
    int fy = fy16 & 0xFFFF;

    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= iw) x1 = iw - 1;
    if (y1 >= ih) y1 = ih - 1;
    if (x0 >= iw) x0 = iw - 1;
    if (y0 >= ih) y0 = ih - 1;

    uint32_t c00 = pixels[y0 * iw + x0];
    uint32_t c10 = pixels[y0 * iw + x1];
    uint32_t c01 = pixels[y1 * iw + x0];
    uint32_t c11 = pixels[y1 * iw + x1];

    int ifx = 0x10000 - fx;
    int ify = 0x10000 - fy;

    int r = (int)( (((c00>>16)&0xFF)*ifx + ((c10>>16)&0xFF)*fx) >> 16 ) * ify
          + (int)( (((c01>>16)&0xFF)*ifx + ((c11>>16)&0xFF)*fx) >> 16 ) * fy;
    int g = (int)( (((c00>>8)&0xFF)*ifx + ((c10>>8)&0xFF)*fx) >> 16 ) * ify
          + (int)( (((c01>>8)&0xFF)*ifx + ((c11>>8)&0xFF)*fx) >> 16 ) * fy;
    int b = (int)( ((c00&0xFF)*ifx + (c10&0xFF)*fx) >> 16 ) * ify
          + (int)( ((c01&0xFF)*ifx + (c11&0xFF)*fx) >> 16 ) * fy;

    r >>= 16; g >>= 16; b >>= 16;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

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

/* Restore a rectangle from the splash image (or fill with COL_BG if no image) */
static void splash_restore_rect(int x, int y, int w, int h) {
    if (!splash_img.pixels || splash_img.width <= 0) {
        splash_fill_rect(x, y, w, h, COL_BG);
        return;
    }
    uint8_t* fb = (uint8_t*)fb_get_buffer();
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();
    uint32_t pitch = fb_pitch();
    int iw = splash_img.width, ih = splash_img.height;

    if (!fb || fb_bpp() != 32) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > scr_w) w = scr_w - x;
    if (y + h > scr_h) h = scr_h - y;
    if (w <= 0 || h <= 0) return;

    for (int row = y; row < y + h; row++) {
        int fy16 = (int)((long)(row + splash_sy_off) * splash_scale_den * 65536 / splash_scale_num);
        uint32_t* line = (uint32_t*)(fb + row * pitch);
        for (int col = x; col < x + w; col++) {
            int fx16 = (int)((long)(col + splash_sx_off) * splash_scale_den * 65536 / splash_scale_num);
            line[col] = bilinear_sample(splash_img.pixels, iw, ih, fx16, fy16);
        }
    }
}

/* ---- Redraw ---- */

/* Cached layout positions so we can clear only what changes */
static int splash_logo_x, splash_logo_y;
static int splash_msg_y;
static int splash_bar_x, splash_bar_y;

static void splash_redraw_full(void) {
    int scr_w = (int)fb_width();
    int scr_h = (int)fb_height();

    /* Full background */
    splash_fill_rect(0, 0, scr_w, scr_h, COL_BG);

    /* Try loading splash image (once) */
    if (!splash_img_loaded) {
        splash_img_loaded = 1;
        if (png_load(splash_png_data, splash_png_size, &splash_img) != 0)
            splash_img.pixels = 0;
    }

    /* Blit splash image centered/scaled on screen */
    if (splash_img.pixels && splash_img.width > 0 && splash_img.height > 0) {
        uint8_t* fb = (uint8_t*)fb_get_buffer();
        uint32_t pitch = fb_pitch();
        int iw = splash_img.width;
        int ih = splash_img.height;
        int scale_num, scale_den, sx_off = 0, sy_off = 0;

        if (iw * scr_h > ih * scr_w) {
            scale_num = scr_h;  scale_den = ih;
            sx_off = ((int)((long)iw * scale_num / scale_den) - scr_w) / 2;
        } else {
            scale_num = scr_w;  scale_den = iw;
            sy_off = ((int)((long)ih * scale_num / scale_den) - scr_h) / 2;
        }

        /* Cache for splash_restore_rect */
        splash_scale_num = scale_num;
        splash_scale_den = scale_den;
        splash_sx_off = sx_off;
        splash_sy_off = sy_off;

        for (int y = 0; y < scr_h; y++) {
            int fy16 = (int)((long)(y + sy_off) * scale_den * 65536 / scale_num);
            uint32_t* line = (uint32_t*)(fb + y * pitch);
            for (int x = 0; x < scr_w; x++) {
                int fx16 = (int)((long)(x + sx_off) * scale_den * 65536 / scale_num);
                line[x] = bilinear_sample(splash_img.pixels, iw, ih, fx16, fy16);
            }
        }
    }

    /* Cache layout positions — bottom of screen */
    splash_bar_x  = (scr_w - BAR_WIDTH) / 2;
    splash_bar_y  = scr_h - 40;
    splash_msg_y  = splash_bar_y - FONT_PSF_HEIGHT - 8;
    splash_logo_x = (scr_w - splash_string_width(LOGO_TEXT)) / 2;
    splash_logo_y = splash_msg_y - FONT_PSF_HEIGHT - 12;

    /* Logo text at bottom area */
    splash_draw_string(splash_logo_x, splash_logo_y, LOGO_TEXT, COL_LOGO);
}

static void splash_redraw(void) {
    int scr_w = (int)fb_width();

    /* Restore message area from splash image, then redraw text */
    splash_restore_rect(0, splash_msg_y, scr_w, FONT_PSF_HEIGHT);
    if (splash_msg[0]) {
        int mx = (scr_w - splash_string_width(splash_msg)) / 2;
        splash_draw_string(mx, splash_msg_y, splash_msg, COL_MSG);
    }

    /* Restore progress bar area, then redraw bar */
    splash_restore_rect(splash_bar_x, splash_bar_y, BAR_WIDTH, BAR_HEIGHT);
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
