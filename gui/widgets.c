/*
 * Lyth OS — Widget Kit Implementation
 *
 * Centralized widget drawing, event dispatch, and keyboard navigation.
 * All widgets render onto the owning window's surface using theme colours.
 */

#include "widgets.h"
#include "window.h"
#include "theme.h"
#include "string.h"

/* ==================================================================
 *  Internal helpers
 * ================================================================== */

static void sf_fill(gui_surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    gui_surface_fill(s, x, y, w, h, c);
}

static void sf_hline(gui_surface_t *s, int x, int y, int w, uint32_t c)
{
    gui_surface_hline(s, x, y, w, c);
}

/* Rounded rect (radius 2) filled */
static void sf_rrect(gui_surface_t *s, int x, int y, int w, int h, uint32_t c)
{
    if (h < 4 || w < 4) { sf_fill(s, x, y, w, h, c); return; }
    sf_fill(s, x + 2, y,     w - 4, 1, c);
    sf_fill(s, x + 1, y + 1, w - 2, 1, c);
    sf_fill(s, x,     y + 2, w,     h - 4, c);
    sf_fill(s, x + 1, y + h - 2, w - 2, 1, c);
    sf_fill(s, x + 2, y + h - 1, w - 4, 1, c);
}

/* Outline rounded rect (radius 2) */
static void sf_rrect_outline(gui_surface_t *s, int x, int y, int w, int h,
                             uint32_t c)
{
    if (h < 4 || w < 4) {
        sf_hline(s, x, y, w, c);
        sf_hline(s, x, y + h - 1, w, c);
        sf_fill(s, x, y, 1, h, c);
        sf_fill(s, x + w - 1, y, 1, h, c);
        return;
    }
    /* top */
    sf_hline(s, x + 2, y, w - 4, c);
    gui_surface_putpixel(s, x + 1, y + 1, c);
    gui_surface_putpixel(s, x + w - 2, y + 1, c);
    /* sides */
    sf_fill(s, x, y + 2, 1, h - 4, c);
    sf_fill(s, x + w - 1, y + 2, 1, h - 4, c);
    /* bottom */
    gui_surface_putpixel(s, x + 1, y + h - 2, c);
    gui_surface_putpixel(s, x + w - 2, y + h - 2, c);
    sf_hline(s, x + 2, y + h - 1, w - 4, c);
}

/* Small filled circle (r<=6) for radio buttons */
static void sf_circle(gui_surface_t *s, int cx, int cy, int r, uint32_t c)
{
    int dy, dx, r2 = r * r;
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r2)
                gui_surface_putpixel(s, cx + dx, cy + dy, c);
}

/* Circle outline */
static void sf_circle_outline(gui_surface_t *s, int cx, int cy, int r,
                              uint32_t c)
{
    int dy, dx, r2 = r * r, ri2 = (r - 1) * (r - 1);
    for (dy = -r; dy <= r; dy++)
        for (dx = -r; dx <= r; dx++) {
            int d = dx * dx + dy * dy;
            if (d <= r2 && d >= ri2)
                gui_surface_putpixel(s, cx + dx, cy + dy, c);
        }
}

/* Allocate a wid_t slot in the window */
static wid_t *wid_alloc(gui_window_t *win)
{
    wid_t *w;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    w = &win->widgets[win->widget_count++];
    memset(w, 0, sizeof(*w));
    w->state = WID_DEFAULT;
    return w;
}

/* Safe string copy into wid_t.text (63 chars max) */
static void wid_set_text(wid_t *w, const char *src)
{
    int len;
    if (!src) { w->text[0] = '\0'; return; }
    len = (int)strlen(src);
    if (len > 63) len = 63;
    memcpy(w->text, src, (size_t)len);
    w->text[len] = '\0';
}

/* Parse "|"-separated string into w->items[], set w->item_count */
static void wid_parse_items(wid_t *w, const char *src)
{
    int i = 0, j = 0;
    w->item_count = 0;
    if (!src) return;
    while (*src && i < WID_MAX_ITEMS) {
        if (*src == '|') {
            w->items[i][j] = '\0';
            i++; j = 0;
        } else {
            if (j < WID_ITEM_LEN - 1)
                w->items[i][j++] = *src;
        }
        src++;
    }
    if (j > 0 || i < WID_MAX_ITEMS) {
        w->items[i][j] = '\0';
        i++;
    }
    w->item_count = (int16_t)i;
}

/* Resolve fg/bg — use custom if nonzero, otherwise theme default */
static uint32_t resolve_fg(wid_t *w)
{
    return w->fg ? w->fg : THEME_COL_TEXT;
}

static uint32_t resolve_bg(wid_t *w)
{
    return w->bg ? w->bg : THEME_COL_SURFACE0;
}

/* ==================================================================
 *  Per-type draw functions
 *
 *  (ox, oy) = content-area origin in surface coordinates.
 * ================================================================== */

static void draw_label(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    uint32_t col = (w->state & WID_ENABLED) ? resolve_fg(w) : THEME_COL_DIM;
    gui_surface_draw_string(s, ox + w->x, oy + w->y, w->text, col, 0, 0);
}

static void draw_button(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    uint32_t bg, fg, border;

    if (!(w->state & WID_ENABLED)) {
        bg = THEME_COL_SURFACE0;
        fg = THEME_COL_DIM;
        border = THEME_COL_BORDER;
    } else if (w->state & WID_PRESSED) {
        bg = THEME_COL_FOCUS;
        fg = THEME_COL_CRUST;
        border = THEME_COL_FOCUS;
    } else if (w->state & WID_HOVERED) {
        bg = THEME_COL_SURFACE1;
        fg = THEME_COL_TEXT;
        border = THEME_COL_ACCENT;
    } else {
        bg = resolve_bg(w);
        fg = resolve_fg(w);
        border = THEME_COL_BORDER;
    }

    sf_rrect(s, bx, by, bw, bh, bg);
    sf_rrect_outline(s, bx, by, bw, bh, border);

    /* center text */
    {
        int len = (int)strlen(w->text);
        int tx = bx + (bw - len * THEME_FONT_W) / 2;
        int ty = by + (bh - THEME_FONT_H) / 2;
        gui_surface_draw_string(s, tx, ty, w->text, fg, 0, 0);
    }
}

/* Checkbox: 14×14 box + label */
#define CHK_SIZE 14

static void draw_checkbox(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int cy = by + (w->height - CHK_SIZE) / 2;
    uint32_t box_bg, box_border, fg;

    if (!(w->state & WID_ENABLED)) {
        box_bg = THEME_COL_CRUST;
        box_border = THEME_COL_BORDER;
        fg = THEME_COL_DIM;
    } else if (w->state & WID_HOVERED) {
        box_bg = THEME_COL_SURFACE1;
        box_border = THEME_COL_ACCENT;
        fg = THEME_COL_TEXT;
    } else {
        box_bg = THEME_COL_CRUST;
        box_border = THEME_COL_BORDER;
        fg = THEME_COL_TEXT;
    }

    sf_rrect(s, bx, cy, CHK_SIZE, CHK_SIZE, box_bg);
    sf_rrect_outline(s, bx, cy, CHK_SIZE, CHK_SIZE, box_border);

    /* checkmark */
    if (w->state & WID_CHECKED) {
        uint32_t chk = THEME_COL_ACCENT;
        /* simple check: two lines */
        gui_surface_putpixel(s, bx + 3,  cy + 7, chk);
        gui_surface_putpixel(s, bx + 4,  cy + 8, chk);
        gui_surface_putpixel(s, bx + 5,  cy + 9, chk);
        gui_surface_putpixel(s, bx + 6,  cy + 8, chk);
        gui_surface_putpixel(s, bx + 7,  cy + 7, chk);
        gui_surface_putpixel(s, bx + 8,  cy + 6, chk);
        gui_surface_putpixel(s, bx + 9,  cy + 5, chk);
        gui_surface_putpixel(s, bx + 10, cy + 4, chk);
        /* thicken */
        gui_surface_putpixel(s, bx + 3,  cy + 8, chk);
        gui_surface_putpixel(s, bx + 4,  cy + 9, chk);
        gui_surface_putpixel(s, bx + 5,  cy + 10, chk);
        gui_surface_putpixel(s, bx + 6,  cy + 9, chk);
        gui_surface_putpixel(s, bx + 7,  cy + 8, chk);
        gui_surface_putpixel(s, bx + 8,  cy + 7, chk);
        gui_surface_putpixel(s, bx + 9,  cy + 6, chk);
        gui_surface_putpixel(s, bx + 10, cy + 5, chk);
    }

    /* label */
    {
        int tx = bx + CHK_SIZE + 6;
        int ty = by + (w->height - THEME_FONT_H) / 2;
        gui_surface_draw_string(s, tx, ty, w->text, fg, 0, 0);
    }
}

/* Radio: 12px diameter circle + label */
#define RADIO_R 6

static void draw_radio(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int cx = bx + RADIO_R;
    int cy = by + w->height / 2;
    uint32_t fg, ring;

    if (!(w->state & WID_ENABLED)) {
        ring = THEME_COL_BORDER;
        fg = THEME_COL_DIM;
    } else if (w->state & WID_HOVERED) {
        ring = THEME_COL_ACCENT;
        fg = THEME_COL_TEXT;
    } else {
        ring = THEME_COL_OVERLAY1;
        fg = THEME_COL_TEXT;
    }

    sf_circle_outline(s, cx, cy, RADIO_R, ring);
    if (w->state & WID_CHECKED) {
        sf_circle(s, cx, cy, 3, THEME_COL_ACCENT);
    }

    /* label */
    {
        int tx = bx + RADIO_R * 2 + 6;
        int ty = by + (w->height - THEME_FONT_H) / 2;
        gui_surface_draw_string(s, tx, ty, w->text, fg, 0, 0);
    }
}

/* Slider: track + thumb */
#define SLIDER_TRACK_H  4
#define SLIDER_THUMB_W  12
#define SLIDER_THUMB_H  16

static void draw_slider(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    int track_y = by + (bh - SLIDER_TRACK_H) / 2;
    int range = w->max_val - w->min_val;
    int usable = bw - SLIDER_THUMB_W;
    int thumb_x;
    uint32_t track_bg, fill_col, thumb_col;

    if (range <= 0) range = 1;
    thumb_x = bx + (usable * (w->value - w->min_val)) / range;

    track_bg = THEME_COL_CRUST;
    fill_col = THEME_COL_ACCENT;
    thumb_col = (w->state & WID_PRESSED) ? THEME_COL_ACCENT_HOVER :
                (w->state & WID_HOVERED) ? THEME_COL_TEXT : THEME_COL_SUBTEXT1;

    /* track background */
    sf_rrect(s, bx, track_y, bw, SLIDER_TRACK_H, track_bg);
    /* filled portion */
    {
        int fill_w = thumb_x - bx + SLIDER_THUMB_W / 2;
        if (fill_w > 0)
            sf_fill(s, bx, track_y, fill_w, SLIDER_TRACK_H, fill_col);
    }
    /* thumb */
    {
        int ty = by + (bh - SLIDER_THUMB_H) / 2;
        sf_rrect(s, thumb_x, ty, SLIDER_THUMB_W, SLIDER_THUMB_H, thumb_col);
    }
}

/* Text input: bordered rect + text + cursor */
static void draw_textinput(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    uint32_t bg_col, border_col, text_col;
    int focused = (w->state & WID_FOCUSED) ? 1 : 0;

    bg_col = THEME_COL_CRUST;
    border_col = focused ? THEME_COL_BORDER_FOCUS : THEME_COL_BORDER;
    text_col = THEME_COL_TEXT;

    sf_fill(s, bx, by, bw, bh, bg_col);
    sf_rrect_outline(s, bx, by, bw, bh, border_col);

    /* text or placeholder */
    {
        int tx = bx + 6;
        int ty = by + (bh - THEME_FONT_H) / 2;
        int max_chars = (bw - 12) / THEME_FONT_W;
        if (max_chars < 0) max_chars = 0;
        if (w->text[0]) {
            gui_surface_draw_string_n(s, tx, ty, w->text, max_chars,
                                      text_col, 0, 0);
            /* cursor */
            if (focused) {
                int clen = (int)strlen(w->text);
                if (clen > max_chars) clen = max_chars;
                sf_fill(s, tx + clen * THEME_FONT_W, ty, 2, THEME_FONT_H,
                        THEME_COL_CURSOR);
            }
        } else {
            /* placeholder text (dimmed) */
            gui_surface_draw_string_n(s, tx, ty, "(input)", max_chars,
                                      THEME_COL_DIM, 0, 0);
            if (focused) {
                sf_fill(s, tx, ty, 2, THEME_FONT_H, THEME_COL_CURSOR);
            }
        }
    }
}

static void draw_panel(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    uint32_t bg = w->bg ? w->bg : THEME_COL_MANTLE;
    sf_rrect(s, ox + w->x, oy + w->y, w->width, w->height, bg);
}

static void draw_separator(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    sf_hline(s, ox + w->x, oy + w->y + w->height / 2,
             w->width, THEME_COL_BORDER_DIM);
}

/* Progress: track + filled bar + optional percentage text */
#define PROGRESS_H 8

static void draw_progress(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    int track_y = by + (bh - PROGRESS_H) / 2;
    int fill_w;
    int val = w->value;

    if (val < 0)   val = 0;
    if (val > 100) val = 100;
    fill_w = (bw * val) / 100;

    sf_rrect(s, bx, track_y, bw, PROGRESS_H, THEME_COL_CRUST);
    if (fill_w > 0)
        sf_rrect(s, bx, track_y, fill_w, PROGRESS_H, THEME_COL_ACCENT);
}

/* Switch/toggle: pill shape, ON slides right + accent fill */
#define SWITCH_W  36
#define SWITCH_H  18
#define SWITCH_DOT 12

static void draw_switch(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int pill_y = by + (w->height - SWITCH_H) / 2;
    int on = (w->state & WID_CHECKED) ? 1 : 0;
    uint32_t bg_col, dot_col, fg;

    if (!(w->state & WID_ENABLED)) {
        bg_col = THEME_COL_SURFACE0;
        dot_col = THEME_COL_DIM;
        fg = THEME_COL_DIM;
    } else if (on) {
        bg_col = (w->state & WID_HOVERED) ? THEME_COL_ACCENT_HOVER : THEME_COL_ACCENT;
        dot_col = THEME_COL_CRUST;
        fg = THEME_COL_TEXT;
    } else {
        bg_col = (w->state & WID_HOVERED) ? THEME_COL_SURFACE1 : THEME_COL_SURFACE0;
        dot_col = THEME_COL_SUBTEXT1;
        fg = THEME_COL_TEXT;
    }

    /* pill background */
    sf_rrect(s, bx, pill_y, SWITCH_W, SWITCH_H, bg_col);
    /* dot */
    {
        int dx = on ? (bx + SWITCH_W - SWITCH_DOT - 3) : (bx + 3);
        int dy = pill_y + (SWITCH_H - SWITCH_DOT) / 2;
        sf_rrect(s, dx, dy, SWITCH_DOT, SWITCH_DOT, dot_col);
    }
    /* label */
    if (w->text[0]) {
        int tx = bx + SWITCH_W + 8;
        int ty = by + (w->height - THEME_FONT_H) / 2;
        gui_surface_draw_string(s, tx, ty, w->text, fg, 0, 0);
    }
}

/* Tabs: horizontal bar with selectable tabs */
#define TAB_PAD_X  12
#define TAB_H      28

static void draw_tabs(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int tab_x = bx;
    int i;

    /* bottom border line */
    sf_hline(s, bx, by + TAB_H - 1, w->width, THEME_COL_BORDER);

    for (i = 0; i < w->item_count; i++) {
        int len = (int)strlen(w->items[i]);
        int tw = len * THEME_FONT_W + TAB_PAD_X * 2;
        int sel = (i == w->sel);
        uint32_t fg, bg_col;

        if (sel) {
            fg = THEME_COL_TEXT;
            bg_col = THEME_COL_BASE;
        } else if (w->state & WID_ENABLED) {
            fg = THEME_COL_SUBTEXT0;
            bg_col = THEME_COL_MANTLE;
        } else {
            fg = THEME_COL_DIM;
            bg_col = THEME_COL_MANTLE;
        }

        sf_fill(s, tab_x, by, tw, TAB_H - 1, bg_col);
        gui_surface_draw_string(s, tab_x + TAB_PAD_X,
                                by + (TAB_H - THEME_FONT_H) / 2,
                                w->items[i], fg, 0, 0);
        /* active indicator — accent line at bottom */
        if (sel)
            sf_fill(s, tab_x, by + TAB_H - 3, tw, 3, THEME_COL_ACCENT);

        tab_x += tw;
    }
}

/* Dropdown: button that opens a popup list */
#define DROP_ARROW_W  16
#define DROP_ITEM_H   24

static void draw_dropdown(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = THEME_BTN_H;
    uint32_t bg_col, border_col, fg;

    if (!(w->state & WID_ENABLED)) {
        bg_col = THEME_COL_SURFACE0; border_col = THEME_COL_BORDER; fg = THEME_COL_DIM;
    } else if (w->state & WID_HOVERED) {
        bg_col = THEME_COL_SURFACE1; border_col = THEME_COL_ACCENT; fg = THEME_COL_TEXT;
    } else {
        bg_col = THEME_COL_CRUST; border_col = THEME_COL_BORDER; fg = THEME_COL_TEXT;
    }

    /* button area */
    sf_rrect(s, bx, by, bw, bh, bg_col);
    sf_rrect_outline(s, bx, by, bw, bh, border_col);

    /* selected text */
    if (w->sel >= 0 && w->sel < w->item_count) {
        gui_surface_draw_string(s, bx + 8, by + (bh - THEME_FONT_H) / 2,
                                w->items[w->sel], fg, 0, 0);
    }
    /* down arrow (simple "v" shape) */
    {
        int ax = bx + bw - DROP_ARROW_W + 2;
        int ay = by + bh / 2 - 2;
        gui_surface_putpixel(s, ax,     ay,     fg);
        gui_surface_putpixel(s, ax + 1, ay + 1, fg);
        gui_surface_putpixel(s, ax + 2, ay + 2, fg);
        gui_surface_putpixel(s, ax + 3, ay + 3, fg);
        gui_surface_putpixel(s, ax + 4, ay + 2, fg);
        gui_surface_putpixel(s, ax + 5, ay + 1, fg);
        gui_surface_putpixel(s, ax + 6, ay,     fg);
        /* thicken */
        gui_surface_putpixel(s, ax,     ay + 1, fg);
        gui_surface_putpixel(s, ax + 1, ay + 2, fg);
        gui_surface_putpixel(s, ax + 2, ay + 3, fg);
        gui_surface_putpixel(s, ax + 4, ay + 3, fg);
        gui_surface_putpixel(s, ax + 5, ay + 2, fg);
        gui_surface_putpixel(s, ax + 6, ay + 1, fg);
    }
}

/* Draw ONLY the popup portion of an open dropdown (used in z-order pass) */
static void draw_dropdown_popup(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = THEME_BTN_H;
    int py = by + bh + 2;
    int popup_h = w->item_count * DROP_ITEM_H + 4;
    int i;

    sf_rrect(s, bx, py, bw, popup_h, THEME_COL_MANTLE);
    sf_rrect_outline(s, bx, py, bw, popup_h, THEME_COL_BORDER);

    for (i = 0; i < w->item_count; i++) {
        int iy = py + 2 + i * DROP_ITEM_H;
        if (i == w->sel) {
            sf_fill(s, bx + 2, iy, bw - 4, DROP_ITEM_H, THEME_COL_SURFACE1);
        }
        gui_surface_draw_string(s, bx + 8,
                                iy + (DROP_ITEM_H - THEME_FONT_H) / 2,
                                w->items[i], THEME_COL_TEXT, 0, 0);
    }
}

/* ListView: scrollable list with selection */
#define LV_ITEM_H   22
#define LV_SCROLL_W  8

static void draw_listview(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    int vis = w->vis_rows;
    int i;

    /* background */
    sf_fill(s, bx, by, bw, bh, THEME_COL_CRUST);
    sf_rrect_outline(s, bx, by, bw, bh, THEME_COL_BORDER);

    /* items */
    for (i = 0; i < vis && (i + w->scroll_off) < w->item_count; i++) {
        int idx = i + w->scroll_off;
        int iy = by + 2 + i * LV_ITEM_H;
        uint32_t fg;

        if (idx == w->sel) {
            sf_fill(s, bx + 2, iy, bw - 4 - LV_SCROLL_W, LV_ITEM_H,
                    THEME_COL_SURFACE1);
            fg = THEME_COL_TEXT;
        } else {
            fg = THEME_COL_SUBTEXT1;
        }
        gui_surface_draw_string(s, bx + 6,
                                iy + (LV_ITEM_H - THEME_FONT_H) / 2,
                                w->items[idx], fg, 0, 0);
    }

    /* scrollbar track */
    if (w->item_count > vis) {
        int sb_x = bx + bw - LV_SCROLL_W - 2;
        int sb_h = bh - 4;
        int thumb_h = (vis * sb_h) / w->item_count;
        int thumb_y = by + 2 + (w->scroll_off * sb_h) / w->item_count;
        if (thumb_h < 8) thumb_h = 8;

        sf_fill(s, sb_x, by + 2, LV_SCROLL_W, sb_h, THEME_COL_SURFACE0);
        sf_rrect(s, sb_x, thumb_y, LV_SCROLL_W, thumb_h, THEME_COL_OVERLAY1);
    }
}

#define SB_WIDTH      14   /* scrollbar track width */
#define SB_MIN_THUMB  16   /* minimum thumb height */

static void draw_scrollbar(gui_surface_t *s, wid_t *w, int ox, int oy)
{
    int bx = ox + w->x, by = oy + w->y;
    int bw = w->width, bh = w->height;
    int range = w->max_val - w->min_val;
    int thumb_h, thumb_y;
    uint32_t thumb_col;

    /* track background */
    sf_rrect(s, bx, by, bw, bh, THEME_COL_SURFACE0);

    if (range <= 0) return;

    /* compute thumb size and position */
    thumb_h = bh / 4;
    if (thumb_h < SB_MIN_THUMB) thumb_h = SB_MIN_THUMB;
    if (thumb_h > bh) thumb_h = bh;

    thumb_y = by + ((w->value - w->min_val) * (bh - thumb_h)) / range;

    /* thumb colour: accent when hovered/pressed, subtle otherwise */
    thumb_col = (w->state & (WID_HOVERED | WID_PRESSED))
                ? THEME_COL_ACCENT : THEME_COL_OVERLAY1;
    sf_rrect(s, bx + 2, thumb_y, bw - 4, thumb_h, thumb_col);
}

/* ==================================================================
 *  Widget creation functions
 * ================================================================== */

wid_t* wid_label(gui_window_t *win, int x, int y,
                 const char *text, uint32_t fg)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_LABEL;
    w->x = (int16_t)x; w->y = (int16_t)y;
    wid_set_text(w, text);
    w->width = (int16_t)((int)strlen(w->text) * THEME_FONT_W);
    w->height = (int16_t)THEME_FONT_H;
    w->fg = fg;
    return w;
}

wid_t* wid_button(gui_window_t *win, int x, int y, int bw, int bh,
                  const char *text, wid_click_fn on_click)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_BUTTON;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)bw; w->height = (int16_t)bh;
    wid_set_text(w, text);
    w->on_click = on_click;
    return w;
}

wid_t* wid_checkbox(gui_window_t *win, int x, int y,
                    const char *label, int checked, wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_CHECKBOX;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)(CHK_SIZE + 6 + (int)strlen(label) * THEME_FONT_W);
    w->height = (int16_t)(CHK_SIZE > THEME_FONT_H ? CHK_SIZE : THEME_FONT_H);
    wid_set_text(w, label);
    if (checked) w->state |= WID_CHECKED;
    w->on_change = on_change;
    return w;
}

wid_t* wid_radio(gui_window_t *win, int x, int y,
                 const char *label, int group, int selected,
                 wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_RADIO;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)(RADIO_R * 2 + 6 + (int)strlen(label) * THEME_FONT_W);
    w->height = (int16_t)(RADIO_R * 2 > THEME_FONT_H ? RADIO_R * 2 : THEME_FONT_H);
    wid_set_text(w, label);
    w->value = group;   /* group id stored in value */
    if (selected) w->state |= WID_CHECKED;
    w->on_change = on_change;
    return w;
}

wid_t* wid_slider(gui_window_t *win, int x, int y, int sw,
                  int min_val, int max_val, int cur_val,
                  wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_SLIDER;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)sw;
    w->height = (int16_t)SLIDER_THUMB_H;
    w->min_val = min_val;
    w->max_val = max_val;
    w->value = cur_val;
    w->on_change = on_change;
    return w;
}

wid_t* wid_textinput(gui_window_t *win, int x, int y, int tw,
                     const char *placeholder)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_TEXTINPUT;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)tw;
    w->height = (int16_t)THEME_INPUT_H;
    /* placeholder goes in text initially; cleared on first key */
    (void)placeholder;
    w->text[0] = '\0';
    return w;
}

wid_t* wid_panel(gui_window_t *win, int x, int y, int pw, int ph,
                 uint32_t bg)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_PANEL;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)pw; w->height = (int16_t)ph;
    w->bg = bg;
    return w;
}

wid_t* wid_separator(gui_window_t *win, int x, int y, int sw)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_SEPARATOR;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)sw;
    w->height = 4;
    return w;
}

wid_t* wid_progress(gui_window_t *win, int x, int y, int pw, int value)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_PROGRESS;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)pw;
    w->height = (int16_t)(PROGRESS_H + 4);
    w->value = value;
    return w;
}

wid_t* wid_switch(gui_window_t *win, int x, int y,
                  const char *label, int on, wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_SWITCH;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)(SWITCH_W + 8 + (label ? (int)strlen(label) * THEME_FONT_W : 0));
    w->height = (int16_t)(SWITCH_H > THEME_FONT_H ? SWITCH_H : THEME_FONT_H);
    wid_set_text(w, label);
    if (on) w->state |= WID_CHECKED;
    w->on_change = on_change;
    return w;
}

wid_t* wid_tabs(gui_window_t *win, int x, int y, int tw,
                const char *items, int selected, wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_TABS;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)tw;
    w->height = (int16_t)TAB_H;
    wid_parse_items(w, items);
    w->sel = (int16_t)selected;
    w->on_change = on_change;
    return w;
}

wid_t* wid_dropdown(gui_window_t *win, int x, int y, int dw,
                    const char *items, int selected, wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_DROPDOWN;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)dw;
    /* height includes potential popup */
    w->height = (int16_t)THEME_BTN_H;
    wid_parse_items(w, items);
    w->sel = (int16_t)selected;
    w->on_change = on_change;
    return w;
}

wid_t* wid_listview(gui_window_t *win, int x, int y, int lw,
                    int vis_rows, const char *items, int selected,
                    wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_LISTVIEW;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)lw;
    w->vis_rows = (int16_t)vis_rows;
    w->height = (int16_t)(vis_rows * LV_ITEM_H + 4);
    wid_parse_items(w, items);
    w->sel = (int16_t)selected;
    w->scroll_off = 0;
    w->on_change = on_change;
    return w;
}

wid_t* wid_scrollbar(gui_window_t *win, int x, int y, int h,
                     int min_val, int max_val, int cur_val,
                     wid_change_fn on_change)
{
    wid_t *w = wid_alloc(win);
    if (!w) return 0;
    w->type = WID_SCROLLBAR;
    w->x = (int16_t)x; w->y = (int16_t)y;
    w->width = (int16_t)SB_WIDTH;
    w->height = (int16_t)h;
    w->min_val = min_val;
    w->max_val = max_val;
    w->value = cur_val;
    w->on_change = on_change;
    return w;
}

int wid_add_item(wid_t *w, const char *item)
{
    int len;
    if (!w || w->item_count >= WID_MAX_ITEMS) return -1;
    len = (int)strlen(item);
    if (len > WID_ITEM_LEN - 1) len = WID_ITEM_LEN - 1;
    memcpy(w->items[w->item_count], item, (size_t)len);
    w->items[w->item_count][len] = '\0';
    w->item_count++;
    return 0;
}

void wid_clear_items(wid_t *w)
{
    if (!w) return;
    w->item_count = 0;
    w->sel = 0;
    w->scroll_off = 0;
}

/* ==================================================================
 *  Drawing
 * ================================================================== */

void wid_draw_all(gui_window_t *win)
{
    gui_surface_t *s;
    int ox, oy, i;

    if (!win) return;
    s = &win->surface;
    if (!s->pixels) return;

    /* content-area origin in surface coords */
    if (win->flags & GUI_WIN_NO_DECOR) {
        ox = 0; oy = 0;
    } else {
        ox = GUI_BORDER_WIDTH;
        oy = GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH;
    }

    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (!(w->state & WID_VISIBLE)) continue;

        switch (w->type) {
        case WID_LABEL:     draw_label(s, w, ox, oy);     break;
        case WID_BUTTON:    draw_button(s, w, ox, oy);    break;
        case WID_CHECKBOX:  draw_checkbox(s, w, ox, oy);  break;
        case WID_RADIO:     draw_radio(s, w, ox, oy);     break;
        case WID_SLIDER:    draw_slider(s, w, ox, oy);    break;
        case WID_TEXTINPUT: draw_textinput(s, w, ox, oy); break;
        case WID_PANEL:     draw_panel(s, w, ox, oy);     break;
        case WID_SEPARATOR: draw_separator(s, w, ox, oy); break;
        case WID_PROGRESS:  draw_progress(s, w, ox, oy);  break;
        case WID_SWITCH:    draw_switch(s, w, ox, oy);    break;
        case WID_TABS:      draw_tabs(s, w, ox, oy);      break;
        case WID_DROPDOWN:  draw_dropdown(s, w, ox, oy);  break;
        case WID_LISTVIEW:  draw_listview(s, w, ox, oy);  break;
        case WID_SCROLLBAR: draw_scrollbar(s, w, ox, oy); break;
        }
    }

    /* Second pass: draw open dropdown popups on top of all other widgets */
    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (!(w->state & WID_VISIBLE)) continue;
        if (w->type == WID_DROPDOWN && (w->state & WID_FOCUSED) && w->item_count > 0)
            draw_dropdown_popup(s, w, ox, oy);
    }
}

/* ==================================================================
 *  Click handling
 * ================================================================== */

static wid_t *hit_wid(gui_window_t *win, int rx, int ry)
{
    int i;
    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (!(w->state & WID_VISIBLE) || !(w->state & WID_ENABLED))
            continue;
        if (rx >= w->x && rx < w->x + w->width &&
            ry >= w->y && ry < w->y + w->height)
            return w;
    }
    return 0;
}

/* Uncheck all radios in the same group, except `except` */
static void radio_uncheck_group(gui_window_t *win, int group, wid_t *except)
{
    int i;
    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (w->type == WID_RADIO && w->value == group && w != except)
            w->state &= (uint16_t)~WID_CHECKED;
    }
}

int wid_handle_click(gui_window_t *win, int rx, int ry, int button)
{
    wid_t *w;
    int i;
    if (!win || button != 1) return 0;
    w = hit_wid(win, rx, ry);

    /* Close any open dropdown that wasn't clicked */
    for (i = 0; i < win->widget_count; i++) {
        wid_t *d = &win->widgets[i];
        if (d->type == WID_DROPDOWN && (d->state & WID_FOCUSED) && d != w) {
            d->state &= (uint16_t)~WID_FOCUSED;
            d->height = (int16_t)THEME_BTN_H;
            win->needs_redraw = 1;
        }
    }

    if (!w) return 0;

    switch (w->type) {
    case WID_BUTTON:
        w->state |= WID_PRESSED;
        if (w->on_click) w->on_click(w);
        w->state &= (uint16_t)~WID_PRESSED;
        win->needs_redraw = 1;
        return 1;

    case WID_CHECKBOX:
        w->state ^= WID_CHECKED;
        if (w->on_change) w->on_change(w, (w->state & WID_CHECKED) ? 1 : 0);
        win->needs_redraw = 1;
        return 1;

    case WID_RADIO:
        if (!(w->state & WID_CHECKED)) {
            radio_uncheck_group(win, w->value, w);
            w->state |= WID_CHECKED;
            if (w->on_change) w->on_change(w, 1);
            win->needs_redraw = 1;
        }
        return 1;

    case WID_SLIDER: {
        int range = w->max_val - w->min_val;
        int local_x = rx - w->x;
        int new_val;
        if (range <= 0) return 1;
        if (local_x < 0) local_x = 0;
        if (local_x >= w->width) local_x = w->width - 1;
        new_val = w->min_val + (local_x * range) / w->width;
        if (new_val != w->value) {
            w->value = new_val;
            if (w->on_change) w->on_change(w, new_val);
            win->needs_redraw = 1;
        }
        return 1;
    }

    case WID_TEXTINPUT: {
        /* focus this input, unfocus others */
        int i;
        for (i = 0; i < win->widget_count; i++) {
            wid_t *other = &win->widgets[i];
            if (other->type == WID_TEXTINPUT || other->type == WID_DROPDOWN)
                other->state &= (uint16_t)~WID_FOCUSED;
        }
        w->state |= WID_FOCUSED;
        win->needs_redraw = 1;
        return 1;
    }

    case WID_SWITCH:
        w->state ^= WID_CHECKED;
        if (w->on_change) w->on_change(w, (w->state & WID_CHECKED) ? 1 : 0);
        win->needs_redraw = 1;
        return 1;

    case WID_TABS: {
        int local_x = rx - w->x;
        int tab_x = 0, i;
        for (i = 0; i < w->item_count; i++) {
            int tw = (int)strlen(w->items[i]) * THEME_FONT_W + TAB_PAD_X * 2;
            if (local_x >= tab_x && local_x < tab_x + tw) {
                if (i != w->sel) {
                    w->sel = (int16_t)i;
                    if (w->on_change) w->on_change(w, i);
                    win->needs_redraw = 1;
                }
                return 1;
            }
            tab_x += tw;
        }
        return 1;
    }

    case WID_DROPDOWN: {
        int open = (w->state & WID_FOCUSED) ? 1 : 0;
        if (!open) {
            /* close other dropdowns, open this one */
            int i;
            for (i = 0; i < win->widget_count; i++) {
                wid_t *other = &win->widgets[i];
                if (other->type == WID_DROPDOWN || other->type == WID_TEXTINPUT)
                    other->state &= (uint16_t)~WID_FOCUSED;
            }
            w->state |= WID_FOCUSED;
            /* expand hit area to include popup */
            w->height = (int16_t)(THEME_BTN_H + 2 + w->item_count * DROP_ITEM_H + 4);
        } else {
            /* click inside popup — select item */
            int local_y = ry - w->y - THEME_BTN_H - 2;
            if (local_y >= 0 && local_y < w->item_count * DROP_ITEM_H) {
                int idx = (local_y - 2) / DROP_ITEM_H;
                if (idx >= 0 && idx < w->item_count && idx != w->sel) {
                    w->sel = (int16_t)idx;
                    if (w->on_change) w->on_change(w, idx);
                }
            }
            /* close */
            w->state &= (uint16_t)~WID_FOCUSED;
            w->height = (int16_t)THEME_BTN_H;
        }
        win->needs_redraw = 1;
        return 1;
    }

    case WID_LISTVIEW: {
        int local_y = ry - w->y - 2;
        if (local_y >= 0) {
            int idx = local_y / LV_ITEM_H + w->scroll_off;
            if (idx >= 0 && idx < w->item_count && idx != w->sel) {
                w->sel = (int16_t)idx;
                if (w->on_change) w->on_change(w, idx);
                win->needs_redraw = 1;
            }
        }
        return 1;
    }

    case WID_SCROLLBAR: {
        int local_y = ry - w->y;
        int range = w->max_val - w->min_val;
        int new_val;
        if (range <= 0) return 1;
        if (local_y < 0) local_y = 0;
        if (local_y >= w->height) local_y = w->height - 1;
        new_val = w->min_val + (local_y * range) / w->height;
        if (new_val != w->value) {
            w->value = new_val;
            if (w->on_change) w->on_change(w, new_val);
            win->needs_redraw = 1;
        }
        return 1;
    }

    default:
        break;
    }
    return 0;
}

/* ==================================================================
 *  Scroll handling
 * ================================================================== */

int wid_handle_scroll(gui_window_t *win, int rx, int ry, int delta)
{
    wid_t *w;
    if (!win) return 0;
    w = hit_wid(win, rx, ry);
    if (!w) return 0;

    if (w->type == WID_LISTVIEW && w->item_count > w->vis_rows) {
        int new_off = w->scroll_off - delta;
        int max_off = w->item_count - w->vis_rows;
        if (new_off < 0) new_off = 0;
        if (new_off > max_off) new_off = max_off;
        if (new_off != w->scroll_off) {
            w->scroll_off = new_off;
            return 1;
        }
    }

    if (w->type == WID_SLIDER) {
        int range = w->max_val - w->min_val;
        int step = range / 20;
        int new_val;
        if (step < 1) step = 1;
        new_val = w->value + delta * step;
        if (new_val < w->min_val) new_val = w->min_val;
        if (new_val > w->max_val) new_val = w->max_val;
        if (new_val != w->value) {
            w->value = new_val;
            if (w->on_change) w->on_change(w, new_val);
            return 1;
        }
    }

    if (w->type == WID_SCROLLBAR) {
        int range = w->max_val - w->min_val;
        int step = range / 20;
        int new_val;
        if (step < 1) step = 1;
        new_val = w->value - delta * step;
        if (new_val < w->min_val) new_val = w->min_val;
        if (new_val > w->max_val) new_val = w->max_val;
        if (new_val != w->value) {
            w->value = new_val;
            if (w->on_change) w->on_change(w, new_val);
            return 1;
        }
    }

    return 0;
}

/* ==================================================================
 *  Keyboard handling
 * ================================================================== */

int wid_handle_key(gui_window_t *win, int event_type, char key)
{
    int i;
    wid_t *focused = 0;

    if (!win) return 0;

    /* find focused textinput */
    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (w->type == WID_TEXTINPUT && (w->state & WID_FOCUSED)) {
            focused = w;
            break;
        }
    }
    if (!focused) return 0;

    /* only handle key-down (event_type 0 = press, 1 = release typically) */
    if (event_type != 0) return 0;

    if (key == '\b') {
        /* backspace */
        int len = (int)strlen(focused->text);
        if (len > 0) {
            focused->text[len - 1] = '\0';
            win->needs_redraw = 1;
        }
        return 1;
    }

    if (key >= 0x20 && key < 0x7F) {
        /* printable */
        int len = (int)strlen(focused->text);
        if (len < 63) {
            focused->text[len] = key;
            focused->text[len + 1] = '\0';
            win->needs_redraw = 1;
        }
        return 1;
    }

    return 0;
}

/* ==================================================================
 *  Hover tracking
 * ================================================================== */

void wid_update_hover(gui_window_t *win, int rx, int ry)
{
    int i, changed = 0;
    if (!win) return;

    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        if (!(w->state & WID_VISIBLE) || !(w->state & WID_ENABLED))
            continue;

        if (rx >= w->x && rx < w->x + w->width &&
            ry >= w->y && ry < w->y + w->height) {
            if (!(w->state & WID_HOVERED)) {
                w->state |= WID_HOVERED;
                changed = 1;
            }
        } else {
            if (w->state & WID_HOVERED) {
                w->state &= (uint16_t)~WID_HOVERED;
                changed = 1;
            }
        }
    }

    if (changed) win->needs_redraw = 1;
}

void wid_clear_hover(gui_window_t *win)
{
    int i;
    if (!win) return;
    for (i = 0; i < win->widget_count; i++) {
        wid_t *w = &win->widgets[i];
        w->state &= (uint16_t)~WID_HOVERED;
    }
}

/* ==================================================================
 *  Utility
 * ================================================================== */

wid_t* wid_find(gui_window_t *win, int id)
{
    int i;
    if (!win) return 0;
    for (i = 0; i < win->widget_count; i++) {
        if (win->widgets[i].id == (int16_t)id)
            return &win->widgets[i];
    }
    return 0;
}

void wid_clear(gui_window_t *win)
{
    if (!win) return;
    win->widget_count = 0;
    memset(win->widgets, 0, sizeof(win->widgets));
}
