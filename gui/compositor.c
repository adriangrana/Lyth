#include "compositor.h"
#include "window.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "input.h"
#include "mouse.h"
#include "physmem.h"
#include "string.h"
#include "timer.h"
#include "usb_hid.h"

#define MENU_BAR_HEIGHT 22

static uint32_t* back_buffer;
static uint32_t back_buffer_phys;
static uint32_t back_buffer_size;
/* Background cache: desktop + all non-dragging windows rendered once per drag.
 * Allows drag frames to skip the expensive full composite. */
static uint32_t* bg_buffer;
static uint32_t bg_buffer_phys;
static int bg_valid;
static int scr_w, scr_h;
static uint32_t scr_pitch;
static int gui_running;
static int mouse_x, mouse_y;
static int need_redraw;

#define COL_DESKTOP_TOP        0x0B1220
#define COL_DESKTOP_MID        0x111A2B
#define COL_DESKTOP_BOT        0x18263C
#define COL_DESKTOP_GLOW       0x213552

#define COL_MENU_BG            0xE9EDF3
#define COL_MENU_TEXT          0x3E4658
#define COL_MENU_TEXT_MUTED    0x6E7687
#define COL_MENU_BORDER        0xC9D1DD

#define COL_DOCK_BG            0x202838
#define COL_DOCK_BORDER        0x39455B
#define COL_DOCK_ITEM          0x2B3447
#define COL_DOCK_ITEM_ACTIVE   0x3A4A66
#define COL_DOCK_ITEM_GLOW     0x8FB7F0

#define COL_WIN_BG             0xF6F8FB
#define COL_WIN_BORDER_ACTIVE  0xB8C3D3
#define COL_WIN_BORDER_INACT   0xD8E0EA
#define COL_WIN_SHADOW_A       0x131A26
#define COL_WIN_SHADOW_B       0x2A3547

#define COL_TITLE_ACTIVE_BG    0xEEF2F7
#define COL_TITLE_INACTIVE_BG  0xF6F8FB
#define COL_TITLE_TEXT_ACTIVE  0x374151
#define COL_TITLE_TEXT_INACT   0x7E8796
#define COL_TITLE_SEPARATOR    0xD5DCE5

#define COL_BTN_CLOSE          0xED8796
#define COL_BTN_MINIMIZE       0xF9E2AF
#define COL_BTN_MAXIMIZE       0xA6E3A1
#define COL_BTN_INACTIVE       0xA6ADC8

#define COL_ACCENT             0x5B8DEF
#define COL_ACCENT_SOFT        0xE7F0FF

#define COL_TEXT               0x394150
#define COL_TEXT_MUTED         0x6F7787
#define COL_TEXT_WHITE         0xFFFFFF
#define COL_TEXT_DANGER        0xD20F39

#define COL_PANEL_BG           0xEDF2F7
#define COL_PANEL_BORDER       0xD4DCE6
#define COL_BUTTON_BG          0x5B8DEF
#define COL_BUTTON_BORDER      0x4B7DDD
#define COL_BUTTON_TEXT        0xFFFFFF

#define CURSOR_W 12
#define CURSOR_H 18

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

static const int corner_inset[7] = { 6, 4, 3, 2, 1, 1, 0 };

static uint32_t mix_rgb(uint32_t a, uint32_t b, int t, int max) {
    uint32_t ar = (a >> 16) & 0xFF;
    uint32_t ag = (a >> 8) & 0xFF;
    uint32_t ab = a & 0xFF;
    uint32_t br = (b >> 16) & 0xFF;
    uint32_t bg = (b >> 8) & 0xFF;
    uint32_t bb = b & 0xFF;
    uint32_t rr = (ar * (uint32_t)(max - t) + br * (uint32_t)t) / (uint32_t)max;
    uint32_t rg = (ag * (uint32_t)(max - t) + bg * (uint32_t)t) / (uint32_t)max;
    uint32_t rb = (ab * (uint32_t)(max - t) + bb * (uint32_t)t) / (uint32_t)max;
    return (rr << 16) | (rg << 8) | rb;
}

static void bb_putpixel(int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < scr_w && y < scr_h) {
        back_buffer[y * scr_w + x] = rgb;
    }
}

static void bb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > scr_w ? scr_w : x + w;
    int y1 = y + h > scr_h ? scr_h : y + h;
    int py;
    int px;

    if (w <= 0 || h <= 0) {
        return;
    }

    for (py = y0; py < y1; py++) {
        for (px = x0; px < x1; px++) {
            back_buffer[py * scr_w + px] = rgb;
        }
    }
}

static void bb_hline(int x, int y, int w, uint32_t rgb) {
    int i;
    if (w <= 0) {
        return;
    }
    for (i = 0; i < w; i++) {
        bb_putpixel(x + i, y, rgb);
    }
}

static void bb_fill_gradient_v(int x, int y, int w, int h,
                               uint32_t top, uint32_t bottom) {
    int py;
    if (w <= 0 || h <= 0) {
        return;
    }
    for (py = 0; py < h; py++) {
        uint32_t c = mix_rgb(top, bottom, py, h > 1 ? h - 1 : 1);
        bb_fill_rect(x, y + py, w, 1, c);
    }
}

static void bb_fill_circle(int cx, int cy, int r, uint32_t rgb) {
    int y;
    int x;
    for (y = -r; y <= r; y++) {
        for (x = -r; x <= r; x++) {
            if (x * x + y * y <= r * r) {
                bb_putpixel(cx + x, cy + y, rgb);
            }
        }
    }
}

static void bb_fill_rrect(int x, int y, int w, int h, uint32_t rgb) {
    int py;
    int inset;

    if (w <= 0 || h <= 0) {
        return;
    }

    for (py = 0; py < h; py++) {
        if (py < 7) {
            inset = corner_inset[py];
        } else if (py >= h - 7) {
            inset = corner_inset[h - 1 - py];
        } else {
            inset = 0;
        }

        if (inset * 2 >= w) {
            continue;
        }

        bb_fill_rect(x + inset, y + py, w - inset * 2, 1, rgb);
    }
}

static void bb_outline_rrect(int x, int y, int w, int h, uint32_t rgb) {
    int py;
    int inset;

    if (w <= 0 || h <= 0) {
        return;
    }

    for (py = 0; py < h; py++) {
        if (py < 7) {
            inset = corner_inset[py];
        } else if (py >= h - 7) {
            inset = corner_inset[h - 1 - py];
        } else {
            inset = 0;
        }

        if (py == 0 || py == h - 1) {
            bb_hline(x + inset, y + py, w - inset * 2, rgb);
        } else {
            bb_putpixel(x + inset, y + py, rgb);
            bb_putpixel(x + w - 1 - inset, y + py, rgb);
        }
    }
}

static void bb_draw_shadow(int x, int y, int w, int h) {
    bb_fill_rrect(x + 2, y + 3, w, h, COL_WIN_SHADOW_A);
    bb_fill_rrect(x + 4, y + 6, w - 1, h - 1, COL_WIN_SHADOW_B);
}

static void bb_draw_char(int x, int y, unsigned char ch, uint32_t fg,
                         uint32_t bg, int draw_bg) {
    int row;
    int col;
    uint8_t bits;

    if (ch >= FONT_PSF_GLYPH_COUNT) {
        ch = '?';
    }

    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        bits = font_psf_data[ch][row];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            if (bits & (0x80u >> col)) {
                bb_putpixel(x + col, y + row, fg);
            } else if (draw_bg) {
                bb_putpixel(x + col, y + row, bg);
            }
        }
    }
}

static void bb_draw_string(int x, int y, const char* str,
                           uint32_t fg, uint32_t bg, int draw_bg) {
    while (*str) {
        bb_draw_char(x, y, (unsigned char)*str, fg, bg, draw_bg);
        x += FONT_PSF_WIDTH;
        str++;
    }
}

static void bb_draw_string_clip(int x, int y, const char* str, int max_chars,
                                uint32_t fg, uint32_t bg, int draw_bg) {
    int i = 0;
    while (*str && i < max_chars) {
        bb_draw_char(x, y, (unsigned char)*str, fg, bg, draw_bg);
        x += FONT_PSF_WIDTH;
        str++;
        i++;
    }
}

static int label_wrap_width(gui_window_t* win, gui_widget_t* label, int content_w) {
    int i;
    int wrap_width = content_w - label->x - 12;

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* candidate = &win->widgets[i];

        if (candidate == label || candidate->type != GUI_WIDGET_PANEL) {
            continue;
        }

        if (label->x >= candidate->x &&
            label->x < candidate->x + candidate->width &&
            label->y >= candidate->y &&
            label->y < candidate->y + candidate->height) {
            int available = candidate->width - (label->x - candidate->x) - 12;
            if (available > 0 && available < wrap_width) {
                wrap_width = available;
            }
        }
    }

    if (wrap_width < 8) {
        wrap_width = 8;
    }

    return wrap_width;
}

static void bb_draw_string_wrap(int x, int y, const char* str, int max_width,
                                uint32_t fg, uint32_t bg, int draw_bg) {
    int max_chars = max_width / FONT_PSF_WIDTH;
    const char* p = str;

    if (max_chars <= 0) {
        max_chars = 1;
    }

    while (*p) {
        int line_chars = 0;
        const char* line_start = p;
        char line[64];
        int out = 0;

        while (*p == ' ') {
            p++;
            line_start = p;
        }

        while (*p) {
            const char* word = p;
            int word_len = 0;

            while (word[word_len] && word[word_len] != ' ') {
                word_len++;
            }

            if (line_chars == 0) {
                if (word_len <= max_chars) {
                    int j;
                    for (j = 0; j < word_len && out < 63; j++) {
                        line[out++] = word[j];
                    }
                    line_chars = word_len;
                    p += word_len;
                } else {
                    int j;
                    for (j = 0; j < max_chars && out < 63; j++) {
                        line[out++] = word[j];
                    }
                    line_chars = max_chars;
                    p += max_chars;
                }
            } else if (line_chars + 1 + word_len <= max_chars) {
                int j;
                if (out < 63) {
                    line[out++] = ' ';
                }
                for (j = 0; j < word_len && out < 63; j++) {
                    line[out++] = word[j];
                }
                line_chars += 1 + word_len;
                p += word_len;
            } else {
                break;
            }

            while (*p == ' ') {
                p++;
            }
        }

        line[out] = '\0';
        if (out == 0 && *line_start) {
            int j;
            for (j = 0; j < max_chars && line_start[j] && out < 63; j++) {
                line[out++] = line_start[j];
            }
            line[out] = '\0';
        }

        bb_draw_string(x, y, line, fg, bg, draw_bg);
        y += FONT_PSF_HEIGHT;
    }
}

static void draw_button_widget(int wx, int wy, gui_widget_t* w) {
    int text_len;
    int text_x;
    int text_y;

    bb_fill_rrect(wx, wy, w->width, w->height,
                  w->bg_color ? w->bg_color : COL_BUTTON_BG);
    bb_outline_rrect(wx, wy, w->width, w->height, COL_BUTTON_BORDER);

    text_len = strlen(w->text);
    text_x = wx + (w->width - text_len * FONT_PSF_WIDTH) / 2;
    text_y = wy + (w->height - FONT_PSF_HEIGHT) / 2;

    bb_draw_string(text_x, text_y, w->text,
                   w->fg_color ? w->fg_color : COL_BUTTON_TEXT,
                   0, 0);
}

static void draw_window(gui_window_t* win) {
    int cx;
    int cy;
    int cw;
    int ch;
    int i;
    int title_chars;
    int title_w;
    int title_x;
    int title_y;
    int close_cx;
    int dots_y;
    uint32_t border;
    uint32_t title_bg;
    uint32_t title_fg;

    if (!(win->flags & GUI_WIN_VISIBLE) || (win->flags & GUI_WIN_MINIMIZED)) {
        return;
    }

    border = (win->flags & GUI_WIN_FOCUSED) ? COL_WIN_BORDER_ACTIVE : COL_WIN_BORDER_INACT;
    title_bg = (win->flags & GUI_WIN_FOCUSED) ? COL_TITLE_ACTIVE_BG : COL_TITLE_INACTIVE_BG;
    title_fg = (win->flags & GUI_WIN_FOCUSED) ? COL_TITLE_TEXT_ACTIVE : COL_TITLE_TEXT_INACT;

    bb_draw_shadow(win->x, win->y, win->width, win->height);
    bb_fill_rrect(win->x, win->y, win->width, win->height, COL_WIN_BG);
    bb_outline_rrect(win->x, win->y, win->width, win->height, border);

    bb_fill_rrect(win->x + 1, win->y + 1,
                  win->width - 2, GUI_TITLEBAR_HEIGHT + 4,
                  title_bg);
    bb_fill_rect(win->x + 1, win->y + GUI_TITLEBAR_HEIGHT,
                 win->width - 2, win->height - GUI_TITLEBAR_HEIGHT - 1,
                 COL_WIN_BG);
    bb_hline(win->x + 10, win->y + GUI_TITLEBAR_HEIGHT,
             win->width - 20, COL_TITLE_SEPARATOR);

    dots_y = win->y + 15;
    close_cx = win->x + 18;
    bb_fill_circle(close_cx, dots_y, 4,
                   (win->flags & GUI_WIN_FOCUSED) ? COL_BTN_CLOSE : COL_BTN_INACTIVE);
    bb_fill_circle(close_cx + 14, dots_y, 4,
                   (win->flags & GUI_WIN_FOCUSED) ? COL_BTN_MINIMIZE : COL_BTN_INACTIVE);
    bb_fill_circle(close_cx + 28, dots_y, 4,
                   (win->flags & GUI_WIN_FOCUSED) ? COL_BTN_MAXIMIZE : COL_BTN_INACTIVE);

    bb_hline(win->x + win->width - 54, win->y + 15, 34,
             (win->flags & GUI_WIN_FOCUSED) ? COL_PANEL_BORDER : COL_WIN_BORDER_INACT);

    title_chars = strlen(win->title);
    title_w = title_chars * FONT_PSF_WIDTH;
    title_x = win->x + (win->width - title_w) / 2;
    if (title_x < win->x + 56) {
        title_x = win->x + 56;
    }
    title_y = win->y + (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2 + 1;
    bb_draw_string_clip(title_x, title_y, win->title,
                        (win->width - 112) / FONT_PSF_WIDTH,
                        title_fg, 0, 0);

    cx = gui_window_content_x(win);
    cy = gui_window_content_y(win);
    cw = gui_window_content_w(win);
    ch = gui_window_content_h(win);
    bb_fill_rect(cx, cy, cw, ch, COL_WIN_BG);

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        int wx = cx + w->x;
        int wy = cy + w->y;

        switch (w->type) {
        case GUI_WIDGET_LABEL:
            bb_draw_string_wrap(wx, wy, w->text,
                                label_wrap_width(win, w, cw),
                                w->fg_color ? w->fg_color : COL_TEXT,
                                0, 0);
            break;
        case GUI_WIDGET_BUTTON:
            draw_button_widget(wx, wy, w);
            break;
        case GUI_WIDGET_PANEL:
            bb_fill_rrect(wx, wy, w->width, w->height,
                          w->bg_color ? w->bg_color : COL_PANEL_BG);
            bb_outline_rrect(wx, wy, w->width, w->height, COL_PANEL_BORDER);
            break;
        }
    }

    if (win->on_paint) {
        win->on_paint(win);
    }
}

static void draw_desktop(void) {
    unsigned int ms;
    unsigned int secs;
    unsigned int mins;
    unsigned int hrs;
    char clock[9];
    int dock_w;
    int dock_x;
    int dock_y;
    int count;
    int i;
    int bx;
    int dock_h;

    bb_fill_gradient_v(0, 0, scr_w, scr_h / 2, COL_DESKTOP_TOP, COL_DESKTOP_MID);
    bb_fill_gradient_v(0, scr_h / 2, scr_w, scr_h - scr_h / 2,
                       COL_DESKTOP_MID, COL_DESKTOP_BOT);

    bb_fill_rrect(scr_w - 280, 72, 220, 120, COL_DESKTOP_GLOW);
    bb_fill_rrect(70, scr_h - 230, 180, 96, mix_rgb(COL_DESKTOP_MID, COL_DESKTOP_GLOW, 1, 3));

    for (i = 0; i < scr_h; i += 48) {
        uint32_t stripe = mix_rgb(COL_DESKTOP_TOP, COL_DESKTOP_BOT,
                                  i, scr_h > 1 ? scr_h - 1 : 1);
        bb_hline(0, i, scr_w, stripe);
    }

    bb_fill_rect(0, 0, scr_w, MENU_BAR_HEIGHT, COL_MENU_BG);
    bb_hline(0, MENU_BAR_HEIGHT - 1, scr_w, COL_MENU_BORDER);
    bb_draw_string(12, 4, "Lyth", COL_MENU_TEXT, 0, 0);
    bb_draw_string(60, 4, "Workspace", COL_MENU_TEXT_MUTED, 0, 0);
    bb_draw_string(154, 4, "Desktop", COL_MENU_TEXT_MUTED, 0, 0);

    ms = timer_get_uptime_ms();
    secs = ms / 1000;
    mins = secs / 60;
    hrs = mins / 60;
    clock[0] = '0' + (hrs / 10) % 10;
    clock[1] = '0' + hrs % 10;
    clock[2] = ':';
    clock[3] = '0' + (mins % 60) / 10;
    clock[4] = '0' + (mins % 60) % 10;
    clock[5] = ':';
    clock[6] = '0' + (secs % 60) / 10;
    clock[7] = '0' + (secs % 60) % 10;
    clock[8] = '\0';
    bb_draw_string(scr_w - 76, 4, clock, COL_MENU_TEXT, 0, 0);

    count = gui_window_count();
    dock_w = 112 + count * 44;
    if (dock_w < 220) {
        dock_w = 220;
    }
    if (dock_w > scr_w - 80) {
        dock_w = scr_w - 80;
    }
    dock_x = (scr_w - dock_w) / 2;
    dock_y = scr_h - GUI_TASKBAR_HEIGHT + 10;
    dock_h = GUI_TASKBAR_HEIGHT - 18;

    bb_fill_rrect(dock_x, dock_y, dock_w, dock_h, COL_DOCK_BG);
    bb_outline_rrect(dock_x, dock_y, dock_w, dock_h, COL_DOCK_BORDER);

    bb_fill_rrect(dock_x + 10, dock_y + 5, 48, dock_h - 10, COL_ACCENT);
    bb_draw_string(dock_x + 18,
                   dock_y + 5 + (dock_h - 10 - FONT_PSF_HEIGHT) / 2,
                   "Lyth", COL_TEXT_WHITE, 0, 0);

    bx = dock_x + 68;
    for (i = 0; i < count; i++) {
        gui_window_t* w = gui_window_get(i);
        if (!w || !(w->flags & GUI_WIN_VISIBLE)) {
            continue;
        }

        bb_fill_rrect(bx, dock_y + 5, 34, dock_h - 10,
                      (w->flags & GUI_WIN_FOCUSED) ? COL_DOCK_ITEM_ACTIVE : COL_DOCK_ITEM);
        bb_outline_rrect(bx, dock_y + 5, 34, dock_h - 10, COL_DOCK_BORDER);
        bb_fill_rect(bx + 9, dock_y + 10, 16, 12,
                     (w->flags & GUI_WIN_FOCUSED) ? COL_DOCK_ITEM_GLOW : COL_ACCENT_SOFT);
        bb_hline(bx + 8, dock_y + dock_h - 6, 18,
                 (w->flags & GUI_WIN_FOCUSED) ? COL_DOCK_ITEM_GLOW : COL_DOCK_BORDER);
        bx += 40;
        if (bx + 34 > dock_x + dock_w - 10) {
            break;
        }
    }
}

static void draw_cursor(void) {
    int py;
    int px;

    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            uint16_t bit = (uint16_t)(1u << (CURSOR_W - 1 - px));
            if (cursor_outline[py] & bit) {
                uint32_t c = (cursor_fill[py] & bit) ? 0xFFFFFF : 0x111111;
                bb_putpixel(mouse_x + px, mouse_y + py, c);
            }
        }
    }
}

static void flip(void) {
    if (!fb_get_buffer()) {
        return;
    }

    fb_present_rgb32(back_buffer, (uint32_t)scr_w, (uint32_t)scr_h, (uint32_t)scr_w);
}

static void sort_windows(gui_window_t** sorted, int count) {
    int i;
    int j;
    for (i = 1; i < count; i++) {
        gui_window_t* key = sorted[i];
        j = i - 1;
        while (j >= 0 && sorted[j]->z_order > key->z_order) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }
}

static void composite(void) {
    int i;
    int count;
    gui_window_t* sorted[GUI_MAX_WINDOWS];

    draw_desktop();

    count = gui_window_count();
    for (i = 0; i < count; i++) {
        sorted[i] = gui_window_get(i);
    }
    sort_windows(sorted, count);
    for (i = 0; i < count; i++) {
        draw_window(sorted[i]);
    }

    draw_cursor();
    flip();
}

/*
 * Render desktop + all windows except skip_win into bg_buffer.
 * Called once at the start of each drag gesture.
 */
static void rebuild_bg(gui_window_t* skip_win) {
    int i;
    int count;
    gui_window_t* sorted[GUI_MAX_WINDOWS];
    uint32_t* saved_bb;

    if (!bg_buffer) {
        return;
    }

    /* Redirect all bb_* drawing to bg_buffer for the duration of this call. */
    saved_bb = back_buffer;
    back_buffer = bg_buffer;

    draw_desktop();

    count = gui_window_count();
    for (i = 0; i < count; i++) {
        sorted[i] = gui_window_get(i);
    }
    sort_windows(sorted, count);
    for (i = 0; i < count; i++) {
        if (sorted[i] != skip_win) {
            draw_window(sorted[i]);
        }
    }

    back_buffer = saved_bb;
    bg_valid = 1;
}

/*
 * Fast drag frame: copy cached background, draw only the dragging window
 * and cursor, then flip. Skips full desktop+windows composite entirely.
 */
static void composite_drag(gui_window_t* drag_win) {
    memcpy(back_buffer, bg_buffer, (size_t)(scr_w * scr_h) * sizeof(uint32_t));
    draw_window(drag_win);
    draw_cursor();
    flip();
}

static gui_window_t* hit_test_window(int mx, int my) {
    int i;
    int count;
    gui_window_t* hit = 0;

    count = gui_window_count();
    for (i = 0; i < count; i++) {
        gui_window_t* w = gui_window_get(i);
        if (!w || !(w->flags & GUI_WIN_VISIBLE) || (w->flags & GUI_WIN_MINIMIZED)) {
            continue;
        }
        if (mx >= w->x && mx < w->x + w->width &&
            my >= w->y && my < w->y + w->height) {
            if (!hit || w->z_order > hit->z_order) {
                hit = w;
            }
        }
    }

    return hit;
}

static int hit_close_button(gui_window_t* win, int mx, int my) {
    int close_cx;
    int close_cy;

    if (!(win->flags & GUI_WIN_CLOSEABLE)) {
        return 0;
    }

    close_cx = win->x + 18;
    close_cy = win->y + 15;
    return (mx >= close_cx - 5 && mx <= close_cx + 5 &&
            my >= close_cy - 5 && my <= close_cy + 5);
}

static int hit_titlebar(gui_window_t* win, int mx, int my) {
    return (mx >= win->x + 1 &&
            mx < win->x + win->width - 1 &&
            my >= win->y + 1 &&
            my < win->y + GUI_TITLEBAR_HEIGHT + 1);
}

static gui_widget_t* hit_widget(gui_window_t* win, int mx, int my) {
    int cx = gui_window_content_x(win);
    int cy = gui_window_content_y(win);
    int i;

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        int wx = cx + w->x;
        int wy = cy + w->y;
        if (mx >= wx && mx < wx + w->width &&
            my >= wy && my < wy + w->height) {
            return w;
        }
    }

    return 0;
}

static void on_close_demo(gui_window_t* win) {
    gui_window_destroy(win);
    need_redraw = 1;
}

static void btn_hello_click(gui_widget_t* w) {
    gui_window_t* popup;
    (void)w;

    popup = gui_window_create("Welcome", 408, 292, 220, 120,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (popup) {
        gui_add_label(popup, 24, 22, "Lyth desktop is ready.", COL_TEXT);
        gui_add_panel(popup, 24, 56, 294, 58, COL_ACCENT_SOFT);
        gui_add_label(popup, 38, 72, "Clean chrome, sober palette and", COL_TEXT_MUTED);
        gui_add_label(popup, 38, 90, "a layout that reads like a real OS.", COL_TEXT_MUTED);
        gui_window_fit_to_content(popup, 30, 30, 340, 170);
        popup->on_close = on_close_demo;
        gui_window_focus(popup);
        need_redraw = 1;
    }
}

static void create_demo_windows(void) {
    gui_window_t* win;
    char arch_line[32];
    char display_line[64];

    memcpy(arch_line, "Kernel target: x86_64", 22);
    arch_line[22] = '\0';

    memcpy(display_line, "Display: ", 9);
    display_line[9] = '\0';
    {
        unsigned int value;
        char digits[12];
        int len;
        int pos = 9;

        value = (unsigned int)fb_width();
        len = 0;
        do {
            digits[len++] = (char)('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (len > 0) {
            display_line[pos++] = digits[--len];
        }
        display_line[pos++] = 'x';

        value = (unsigned int)fb_height();
        len = 0;
        do {
            digits[len++] = (char)('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (len > 0) {
            display_line[pos++] = digits[--len];
        }
        display_line[pos++] = 'x';

        value = (unsigned int)fb_bpp();
        len = 0;
        do {
            digits[len++] = (char)('0' + (value % 10U));
            value /= 10U;
        } while (value != 0U);
        while (len > 0) {
            display_line[pos++] = digits[--len];
        }

        memcpy(display_line + pos, " framebuffer", 13);
        pos += 13;
        display_line[pos] = '\0';
    }

    win = gui_window_create("System Overview", 92, 82, 240, 160,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (win) {
        gui_add_label(win, 24, 22, "Lyth OS", COL_ACCENT);
        gui_add_label(win, 24, 52, arch_line, COL_TEXT);
        gui_add_label(win, 24, 72, display_line, COL_TEXT);
        gui_add_label(win, 24, 92, "Desktop: compositor and window manager", COL_TEXT);
        gui_add_panel(win, 24, 126, 366, 2, COL_PANEL_BORDER);
        gui_add_panel(win, 24, 150, 366, 72, COL_PANEL_BG);
        gui_add_label(win, 40, 170, "The interface now aims for a calmer,", COL_TEXT_MUTED);
        gui_add_label(win, 40, 188, "more professional desktop presentation.", COL_TEXT_MUTED);
        gui_add_button(win, 136, 246, 146, 34, "Open Welcome", btn_hello_click);
        gui_window_fit_to_content(win, 40, 30, 420, 300);
        win->on_close = on_close_demo;
        gui_window_focus(win);
    }

    win = gui_window_create("Notes", 566, 118, 240, 160,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (win) {
        gui_add_label(win, 24, 22, "Design goals", COL_TEXT);
        gui_add_label(win, 24, 54, "Less retro desktop, more current OS feel.", COL_TEXT_MUTED);
        gui_add_label(win, 24, 74, "Clean spacing, restrained accents and stable hierarchy.", COL_TEXT_MUTED);
        gui_add_label(win, 24, 106, "Press ESC to leave the desktop.", COL_TEXT_DANGER);
        gui_add_panel(win, 24, 142, 312, 66, COL_PANEL_BG);
        gui_add_label(win, 38, 162, "The dock and menubar are intentionally quiet,", COL_TEXT_MUTED);
        gui_add_label(win, 38, 180, "so the windows stay the primary focus.", COL_TEXT_MUTED);
        gui_window_fit_to_content(win, 34, 32, 360, 250);
        win->on_close = on_close_demo;
    }
}

void gui_init(void) {
    scr_w = (int)fb_width();
    scr_h = (int)fb_height();
    scr_pitch = fb_pitch();

    back_buffer_size = (uint32_t)(scr_w * scr_h * 4);
    back_buffer_phys = physmem_alloc_region(back_buffer_size, 4096);
    if (!back_buffer_phys) {
        return;
    }

    back_buffer = (uint32_t*)(uintptr_t)back_buffer_phys;
    memset(back_buffer, 0, back_buffer_size);

    /* Allocate background cache for smooth window dragging. */
    bg_buffer_phys = physmem_alloc_region(back_buffer_size, 4096);
    if (bg_buffer_phys) {
        bg_buffer = (uint32_t*)(uintptr_t)bg_buffer_phys;
        memset(bg_buffer, 0, back_buffer_size);
    }
    bg_valid = 0;

    mouse_x = scr_w / 2;
    mouse_y = scr_h / 2;
    need_redraw = 1;
    gui_running = 0;

    fb_hide_mouse_cursor();
    create_demo_windows();
}

void gui_stop(void) {
    gui_running = 0;
}

void gui_request_redraw(void) {
    need_redraw = 1;
}

int gui_is_active(void) {
    return gui_running;
}

/* Minimum milliseconds between rendered frames (~60 fps). */
#define GUI_FRAME_INTERVAL_MS 16U

void gui_run(void) {
    input_event_t ev;
    int desktop_h;
    gui_window_t* dragging_win = 0;
    unsigned int last_frame_ms;

    gui_running = 1;
    need_redraw = 1;
    desktop_h = scr_h - GUI_TASKBAR_HEIGHT;
    last_frame_ms = timer_get_uptime_ms();

    while (gui_running) {
        /* Poll USB HID so keyboard/mouse events arrive */
        usb_hid_poll();

        while (input_poll_event(&ev)) {
            if (ev.device_type == INPUT_DEVICE_KEYBOARD) {
                if ((ev.type == INPUT_EVENT_CHAR && ev.character == 27) ||
                    ev.type == INPUT_EVENT_CTRL_C) {
                    gui_running = 0;
                    break;
                }

                {
                    int i;
                    int count = gui_window_count();
                    for (i = 0; i < count; i++) {
                        gui_window_t* w = gui_window_get(i);
                        if (w && (w->flags & GUI_WIN_FOCUSED) && w->on_key) {
                            w->on_key(w, ev.character);
                            need_redraw = 1;
                        }
                    }
                }
            }

            if (ev.device_type == INPUT_DEVICE_MOUSE) {
                mouse_x += ev.delta_x;
                mouse_y += ev.delta_y;

                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= scr_w) mouse_x = scr_w - 1;
                if (mouse_y >= scr_h) mouse_y = scr_h - 1;

                need_redraw = 1;

                if (dragging_win) {
                    if (ev.buttons & 0x01) {
                        int nx = mouse_x - dragging_win->drag_off_x;
                        int ny = mouse_y - dragging_win->drag_off_y;

                        if (nx < -(dragging_win->width - 40)) nx = -(dragging_win->width - 40);
                        if (ny < MENU_BAR_HEIGHT) ny = MENU_BAR_HEIGHT;
                        if (nx > scr_w - 40) nx = scr_w - 40;
                        if (ny > desktop_h - GUI_TITLEBAR_HEIGHT) ny = desktop_h - GUI_TITLEBAR_HEIGHT;

                        gui_window_move(dragging_win, nx, ny);
                    } else {
                        dragging_win->dragging = 0;
                        dragging_win = 0;
                    }
                    continue;
                }

                if (ev.buttons & 0x01) {
                    gui_window_t* w = hit_test_window(mouse_x, mouse_y);
                    if (w) {
                        gui_window_focus(w);

                        if (hit_close_button(w, mouse_x, mouse_y)) {
                            if (w->on_close) {
                                w->on_close(w);
                            } else {
                                gui_window_destroy(w);
                            }
                            continue;
                        }

                        if ((w->flags & GUI_WIN_DRAGGABLE) &&
                            hit_titlebar(w, mouse_x, mouse_y)) {
                            w->dragging = 1;
                            w->drag_off_x = mouse_x - w->x;
                            w->drag_off_y = mouse_y - w->y;
                            dragging_win = w;
                            continue;
                        }

                        {
                            gui_widget_t* wi = hit_widget(w, mouse_x, mouse_y);
                            if (wi && wi->on_click) {
                                wi->on_click(wi);
                            }
                            if (w->on_click) {
                                int rx = mouse_x - gui_window_content_x(w);
                                int ry = mouse_y - gui_window_content_y(w);
                                w->on_click(w, rx, ry, 1);
                            }
                        }
                    }
                }
            }
        }

        if (need_redraw) {
            if (dragging_win && bg_buffer) {
                /* Fast drag path: rebuild background once, then just blit it
                 * and draw the moving window. No frame cap — every mouse
                 * event gets its own frame, keeping movement fluid. */
                if (!bg_valid) {
                    rebuild_bg(dragging_win);
                }
                need_redraw = 0;
                composite_drag(dragging_win);
            } else {
                /* Normal path: full composite with 60 fps cap. */
                bg_valid = 0;
                {
                    unsigned int now = timer_get_uptime_ms();
                    unsigned int elapsed = now - last_frame_ms;
                    if (elapsed >= GUI_FRAME_INTERVAL_MS) {
                        need_redraw = 0;
                        last_frame_ms = now;
                        composite();
                    }
                }
            }
        }

        __asm__ volatile("hlt");
    }

    {
        int i;
        int count = gui_window_count();
        for (i = count - 1; i >= 0; i--) {
            gui_window_t* w = gui_window_get(i);
            if (w) {
                gui_window_destroy(w);
            }
        }
    }

    if (back_buffer) {
        physmem_free_region(back_buffer_phys, back_buffer_size);
        back_buffer = 0;
        back_buffer_phys = 0;
    }

    if (bg_buffer) {
        physmem_free_region(bg_buffer_phys, back_buffer_size);
        bg_buffer = 0;
        bg_buffer_phys = 0;
    }

    fb_clear();
}