/*
 * Software cursor with save-under and minimal dirty rect invalidation.
 *
 * The cursor is 12x18 pixels. Before drawing, we save the pixels
 * underneath. On erase we restore them. Only the cursor's bounding
 * rect (old + new position) is marked dirty, never the whole screen.
 */

#include "cursor.h"
#include "compositor.h"
#include "string.h"

#define CURSOR_W 12
#define CURSOR_H 18

static int cur_x, cur_y;
static int prev_x, prev_y;
static int drawn_x, drawn_y;   /* position where cursor was actually drawn */
static int scr_w, scr_h;
static int cursor_drawn;

/* saved area under cursor */
static uint32_t save_under[CURSOR_H][CURSOR_W];

/* cursor bitmaps: outline and fill */
static const uint16_t cursor_outline[CURSOR_H] = {
    0x800, 0xC00, 0xE00, 0xF00,
    0xF80, 0xFC0, 0xFE0, 0xFF0,
    0xFF8, 0xFFC, 0xFFE, 0xFC0,
    0xEC0, 0xC60, 0x060, 0x060,
    0x030, 0x030
};

static const uint16_t cursor_fill[CURSOR_H] = {
    0x000, 0x400, 0x600, 0x700,
    0x780, 0x7C0, 0x7E0, 0x7F0,
    0x7F8, 0x7C0, 0x7C0, 0x6C0,
    0x440, 0x060, 0x060, 0x020,
    0x030, 0x000
};

void cursor_init(int sw, int sh) {
    scr_w = sw;
    scr_h = sh;
    cur_x = sw / 2;
    cur_y = sh / 2;
    prev_x = cur_x;
    prev_y = cur_y;
    drawn_x = cur_x;
    drawn_y = cur_y;
    cursor_drawn = 0;
}

void cursor_resize(int new_w, int new_h) {
    scr_w = new_w;
    scr_h = new_h;
    if (cur_x >= scr_w) cur_x = scr_w - 1;
    if (cur_y >= scr_h) cur_y = scr_h - 1;
    cursor_drawn = 0;
}

void cursor_set_pos(int x, int y) {
    prev_x = cur_x;
    prev_y = cur_y;
    cur_x = x;
    cur_y = y;
}

void cursor_get_pos(int* x, int* y) {
    if (x) *x = cur_x;
    if (y) *y = cur_y;
}

void cursor_draw(gui_surface_t* bb) {
    int py, px;
    if (!bb || !bb->pixels) return;

    /* save pixels under cursor and draw */
    for (py = 0; py < CURSOR_H; py++) {
        int sy = cur_y + py;
        for (px = 0; px < CURSOR_W; px++) {
            int sx = cur_x + px;
            if (sx >= 0 && sx < bb->width && sy >= 0 && sy < bb->height) {
                save_under[py][px] = bb->pixels[sy * bb->stride + sx];
            } else {
                save_under[py][px] = 0;
            }

            uint16_t bit = (uint16_t)(1u << (CURSOR_W - 1 - px));
            if (cursor_outline[py] & bit) {
                uint32_t c = (cursor_fill[py] & bit) ? 0xFFFFFF : 0x111111;
                if (sx >= 0 && sx < bb->width && sy >= 0 && sy < bb->height) {
                    bb->pixels[sy * bb->stride + sx] = c;
                }
            }
        }
    }
    drawn_x = cur_x;
    drawn_y = cur_y;
    cursor_drawn = 1;
}

void cursor_erase(gui_surface_t* bb) {
    int py, px;
    if (!cursor_drawn || !bb || !bb->pixels) return;

    /* restore saved pixels at the position where cursor was actually drawn */
    for (py = 0; py < CURSOR_H; py++) {
        int sy = drawn_y + py;
        for (px = 0; px < CURSOR_W; px++) {
            int sx = drawn_x + px;
            if (sx >= 0 && sx < bb->width && sy >= 0 && sy < bb->height) {
                /* only restore if outline bit was set (i.e. we actually wrote there) */
                uint16_t bit = (uint16_t)(1u << (CURSOR_W - 1 - px));
                if (cursor_outline[py] & bit) {
                    bb->pixels[sy * bb->stride + sx] = save_under[py][px];
                }
            }
        }
    }
    cursor_drawn = 0;
}

void cursor_invalidate_old(void) {
    gui_dirty_add(drawn_x, drawn_y, CURSOR_W, CURSOR_H);
}

void cursor_invalidate_new(void) {
    gui_dirty_add(cur_x, cur_y, CURSOR_W, CURSOR_H);
}
