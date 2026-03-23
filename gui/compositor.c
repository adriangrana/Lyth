#include "compositor.h"
#include "window.h"
#include "fbconsole.h"
#include "font_psf.h"
#include "input.h"
#include "mouse.h"
#include "heap.h"
#include "string.h"
#include "timer.h"

/* ---- screen state ---- */
static uint32_t* back_buffer;
static int scr_w, scr_h;
static uint32_t scr_pitch;
static int gui_running;
static int mouse_x, mouse_y;
static int need_redraw;
static int drag_window_idx;  /* -1 = none */

/* ---- mouse cursor (arrow, 8x12) ---- */
#define CURSOR_W 8
#define CURSOR_H 12
static const uint8_t cursor_mask[CURSOR_H] = {
    0x80, 0xC0, 0xE0, 0xF0,
    0xF8, 0xFC, 0xFE, 0xFC,
    0xD8, 0x88, 0x0C, 0x04
};
static const uint8_t cursor_fill[CURSOR_H] = {
    0x80, 0x40, 0x20, 0x10,
    0x08, 0x04, 0x02, 0x04,
    0x48, 0x88, 0x08, 0x00
};

/* ======== back-buffer primitives ======== */

static void bb_putpixel(int x, int y, uint32_t rgb) {
    if (x >= 0 && y >= 0 && x < scr_w && y < scr_h)
        back_buffer[y * scr_w + x] = rgb;
}

static void bb_fill_rect(int x, int y, int w, int h, uint32_t rgb) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > scr_w ? scr_w : (x + w);
    int y1 = (y + h) > scr_h ? scr_h : (y + h);
    int py, px;
    for (py = y0; py < y1; py++)
        for (px = x0; px < x1; px++)
            back_buffer[py * scr_w + px] = rgb;
}

static void bb_hline(int x, int y, int w, uint32_t rgb) {
    int i;
    for (i = 0; i < w; i++) bb_putpixel(x + i, y, rgb);
}

static void bb_vline(int x, int y, int h, uint32_t rgb) {
    int i;
    for (i = 0; i < h; i++) bb_putpixel(x, y + i, rgb);
}

static void bb_draw_char(int x, int y, unsigned char ch, uint32_t fg,
                         uint32_t bg, int draw_bg) {
    int row, col;
    uint8_t bits;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        bits = font_psf_data[ch][row];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            if (bits & (0x80u >> col))
                bb_putpixel(x + col, y + row, fg);
            else if (draw_bg)
                bb_putpixel(x + col, y + row, bg);
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

/* Win95-style 3D edges */
static void bb_raised(int x, int y, int w, int h) {
    bb_hline(x, y, w, GUI_COL_BORDER_LIGHT);
    bb_vline(x, y, h, GUI_COL_BORDER_LIGHT);
    bb_hline(x, y + h - 1, w, GUI_COL_BORDER_DARK);
    bb_vline(x + w - 1, y, h, GUI_COL_BORDER_DARK);
}

static void bb_sunken(int x, int y, int w, int h) {
    bb_hline(x, y, w, GUI_COL_BORDER_DARK);
    bb_vline(x, y, h, GUI_COL_BORDER_DARK);
    bb_hline(x, y + h - 1, w, GUI_COL_BORDER_LIGHT);
    bb_vline(x + w - 1, y, h, GUI_COL_BORDER_LIGHT);
}

/* ======== render window decorations ======== */

static void draw_window(gui_window_t* win) {
    int cx, cy, cw, ch;
    int i, tx, ty;
    uint32_t title_bg;
    int close_x, close_y, close_sz;

    if (!(win->flags & GUI_WIN_VISIBLE)) return;
    if (win->flags & GUI_WIN_MINIMIZED) return;

    /* outer border (raised) */
    bb_fill_rect(win->x, win->y, win->width, win->height, GUI_COL_WINDOW_BG);
    bb_raised(win->x, win->y, win->width, win->height);

    /* inner border line */
    bb_hline(win->x + 1, win->y + 1, win->width - 2, 0xFFFFFF);
    bb_vline(win->x + 1, win->y + 1, win->height - 2, 0xFFFFFF);
    bb_hline(win->x + 1, win->y + win->height - 2, win->width - 2, 0x404040);
    bb_vline(win->x + win->width - 2, win->y + 1, win->height - 2, 0x404040);

    /* title bar */
    title_bg = (win->flags & GUI_WIN_FOCUSED) ? GUI_COL_TITLEBAR : GUI_COL_TITLEBAR_IA;
    bb_fill_rect(win->x + GUI_BORDER_WIDTH,
                 win->y + GUI_BORDER_WIDTH,
                 win->width - GUI_BORDER_WIDTH * 2,
                 GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH,
                 title_bg);

    /* title text (centred vertically in title bar) */
    tx = win->x + GUI_BORDER_WIDTH + 4;
    ty = win->y + GUI_BORDER_WIDTH + (GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH - FONT_PSF_HEIGHT) / 2;
    bb_draw_string(tx, ty, win->title, GUI_COL_TITLE_TEXT, title_bg, 0);

    /* close button */
    if (win->flags & GUI_WIN_CLOSEABLE) {
        close_sz = GUI_CLOSE_BTN_SIZE;
        close_x = win->x + win->width - GUI_BORDER_WIDTH - close_sz - 2;
        close_y = win->y + GUI_BORDER_WIDTH + (GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH - close_sz) / 2;
        bb_fill_rect(close_x, close_y, close_sz, close_sz, GUI_COL_CLOSE_BG);
        bb_raised(close_x, close_y, close_sz, close_sz);
        /* draw X */
        for (i = 2; i < close_sz - 2; i++) {
            bb_putpixel(close_x + i, close_y + i, GUI_COL_CLOSE_FG);
            bb_putpixel(close_x + close_sz - 1 - i, close_y + i, GUI_COL_CLOSE_FG);
            bb_putpixel(close_x + i + 1, close_y + i, GUI_COL_CLOSE_FG);
            bb_putpixel(close_x + close_sz - i, close_y + i, GUI_COL_CLOSE_FG);
        }
    }

    /* content area */
    cx = gui_window_content_x(win);
    cy = gui_window_content_y(win);
    cw = gui_window_content_w(win);
    ch = gui_window_content_h(win);

    /* sunken content region */
    bb_sunken(cx - 1, cy - 1, cw + 2, ch + 2);

    /* widgets */
    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        int wx = cx + w->x;
        int wy = cy + w->y;

        switch (w->type) {
        case GUI_WIDGET_LABEL:
            bb_draw_string(wx, wy, w->text, w->fg_color, w->bg_color, 0);
            break;
        case GUI_WIDGET_BUTTON:
            bb_fill_rect(wx, wy, w->width, w->height, w->bg_color);
            bb_raised(wx, wy, w->width, w->height);
            {
                int text_len = strlen(w->text);
                int text_x = wx + (w->width - text_len * FONT_PSF_WIDTH) / 2;
                int text_y = wy + (w->height - FONT_PSF_HEIGHT) / 2;
                bb_draw_string(text_x, text_y, w->text,
                               w->fg_color, w->bg_color, 0);
            }
            break;
        case GUI_WIDGET_PANEL:
            bb_fill_rect(wx, wy, w->width, w->height, w->bg_color);
            break;
        }
    }

    /* custom paint callback */
    if (win->on_paint) win->on_paint(win);
}

/* ======== desktop & taskbar ======== */

static void draw_desktop(void) {
    int tb_y;
    bb_fill_rect(0, 0, scr_w, scr_h, GUI_COL_DESKTOP);

    /* taskbar */
    tb_y = scr_h - GUI_TASKBAR_HEIGHT;
    bb_fill_rect(0, tb_y, scr_w, GUI_TASKBAR_HEIGHT, GUI_COL_TASKBAR);
    bb_raised(0, tb_y, scr_w, GUI_TASKBAR_HEIGHT);

    /* start button */
    bb_fill_rect(4, tb_y + 3, 60, GUI_TASKBAR_HEIGHT - 6, GUI_COL_START_BTN);
    bb_raised(4, tb_y + 3, 60, GUI_TASKBAR_HEIGHT - 6);
    bb_draw_string(12, tb_y + 3 + (GUI_TASKBAR_HEIGHT - 6 - FONT_PSF_HEIGHT) / 2,
                   "Lyth", GUI_COL_START_TEXT, GUI_COL_START_BTN, 0);

    /* clock placeholder */
    {
        unsigned int ms = timer_get_uptime_ms();
        unsigned int secs = ms / 1000;
        unsigned int mins = secs / 60;
        unsigned int hrs = mins / 60;
        char clock[9];
        clock[0] = '0' + (hrs / 10) % 10;
        clock[1] = '0' + hrs % 10;
        clock[2] = ':';
        clock[3] = '0' + (mins % 60) / 10;
        clock[4] = '0' + (mins % 60) % 10;
        clock[5] = ':';
        clock[6] = '0' + (secs % 60) / 10;
        clock[7] = '0' + (secs % 60) % 10;
        clock[8] = '\0';
        bb_sunken(scr_w - 76, tb_y + 3, 72, GUI_TASKBAR_HEIGHT - 6);
        bb_draw_string(scr_w - 72, tb_y + 3 + (GUI_TASKBAR_HEIGHT - 6 - FONT_PSF_HEIGHT) / 2,
                       clock, GUI_COL_TASKBAR_TEXT, GUI_COL_TASKBAR, 1);
    }

    /* taskbar window buttons */
    {
        int count = gui_window_count();
        int bx = 70;
        int i;
        for (i = 0; i < count; i++) {
            gui_window_t* w = gui_window_get(i);
            if (!w || !(w->flags & GUI_WIN_VISIBLE)) continue;
            int bw = 120;
            if (bx + bw > scr_w - 80) break;
            if (w->flags & GUI_WIN_FOCUSED)
                bb_sunken(bx, tb_y + 3, bw, GUI_TASKBAR_HEIGHT - 6);
            else {
                bb_fill_rect(bx, tb_y + 3, bw, GUI_TASKBAR_HEIGHT - 6, GUI_COL_BTN_FACE);
                bb_raised(bx, tb_y + 3, bw, GUI_TASKBAR_HEIGHT - 6);
            }
            /* truncate title to fit */
            {
                int max_chars = (bw - 8) / FONT_PSF_WIDTH;
                char buf[16];
                int tlen = strlen(w->title);
                if (tlen > max_chars) tlen = max_chars;
                if (tlen > 15) tlen = 15;
                memcpy(buf, w->title, tlen);
                buf[tlen] = '\0';
                bb_draw_string(bx + 4,
                    tb_y + 3 + (GUI_TASKBAR_HEIGHT - 6 - FONT_PSF_HEIGHT) / 2,
                    buf, GUI_COL_TASKBAR_TEXT, GUI_COL_BTN_FACE, 0);
            }
            bx += bw + 2;
        }
    }
}

/* ======== mouse cursor ======== */

static void draw_cursor(void) {
    int py, px, bit;
    for (py = 0; py < CURSOR_H; py++) {
        for (px = 0; px < CURSOR_W; px++) {
            bit = 0x80u >> px;
            if (cursor_mask[py] & bit) {
                uint32_t c = (cursor_fill[py] & bit) ? 0x000000 : 0xFFFFFF;
                bb_putpixel(mouse_x + px, mouse_y + py, c);
            }
        }
    }
}

/* ======== blit back buffer to framebuffer ======== */

static void flip(void) {
    uint8_t* fb = (uint8_t*)fb_get_buffer();
    int y;
    if (!fb) return;
    for (y = 0; y < scr_h; y++) {
        memcpy(fb + y * scr_pitch,
               back_buffer + y * scr_w,
               scr_w * 4);
    }
}

/* ======== full scene redraw ======== */

static void composite(void) {
    int i, j, count;
    gui_window_t* sorted[GUI_MAX_WINDOWS];

    /* desktop background & taskbar */
    draw_desktop();

    /* sort windows by z_order (simple insertion sort) */
    count = gui_window_count();
    for (i = 0; i < count; i++)
        sorted[i] = gui_window_get(i);
    for (i = 1; i < count; i++) {
        gui_window_t* key = sorted[i];
        j = i - 1;
        while (j >= 0 && sorted[j]->z_order > key->z_order) {
            sorted[j + 1] = sorted[j];
            j--;
        }
        sorted[j + 1] = key;
    }

    /* draw windows back-to-front */
    for (i = 0; i < count; i++)
        draw_window(sorted[i]);

    /* mouse cursor on top */
    draw_cursor();

    /* blit to framebuffer */
    flip();
}

/* ======== hit testing ======== */

static gui_window_t* hit_test_window(int mx, int my) {
    int i, count;
    gui_window_t* sorted[GUI_MAX_WINDOWS];
    gui_window_t* hit = 0;

    count = gui_window_count();
    for (i = 0; i < count; i++)
        sorted[i] = gui_window_get(i);

    /* check front-to-back (reverse z-order) */
    for (i = 0; i < count; i++) {
        gui_window_t* w = sorted[i];
        if (!(w->flags & GUI_WIN_VISIBLE)) continue;
        if (w->flags & GUI_WIN_MINIMIZED) continue;
        if (mx >= w->x && mx < w->x + w->width &&
            my >= w->y && my < w->y + w->height) {
            if (!hit || w->z_order > hit->z_order)
                hit = w;
        }
    }
    return hit;
}

static int hit_close_button(gui_window_t* win, int mx, int my) {
    int close_sz, close_x, close_y;
    if (!(win->flags & GUI_WIN_CLOSEABLE)) return 0;
    close_sz = GUI_CLOSE_BTN_SIZE;
    close_x = win->x + win->width - GUI_BORDER_WIDTH - close_sz - 2;
    close_y = win->y + GUI_BORDER_WIDTH +
              (GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH - close_sz) / 2;
    return (mx >= close_x && mx < close_x + close_sz &&
            my >= close_y && my < close_y + close_sz);
}

static int hit_titlebar(gui_window_t* win, int mx, int my) {
    return (mx >= win->x + GUI_BORDER_WIDTH &&
            mx < win->x + win->width - GUI_BORDER_WIDTH &&
            my >= win->y + GUI_BORDER_WIDTH &&
            my < win->y + GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH);
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
            my >= wy && my < wy + w->height)
            return w;
    }
    return 0;
}

/* ======== demo windows ======== */

static void on_close_demo(gui_window_t* win) {
    gui_window_destroy(win);
    need_redraw = 1;
}

static void btn_hello_click(gui_widget_t* w) {
    (void)w;
    /* create a small popup */
    gui_window_t* popup = gui_window_create("Mensaje", 400, 300, 240, 120,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (popup) {
        gui_add_label(popup, 20, 16, "Hola desde Lyth OS!", 0x000000);
        popup->on_close = on_close_demo;
        gui_window_focus(popup);
        need_redraw = 1;
    }
}

static void create_demo_windows(void) {
    gui_window_t* win;

    /* System info window */
    win = gui_window_create("Sistema", 80, 60, 340, 260,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (win) {
        gui_add_label(win, 12, 12, "Lyth OS v0.1", 0x000080);
        gui_add_label(win, 12, 34, "Kernel: x86 (i686)", 0x000000);
        gui_add_label(win, 12, 54, "Video: 1280x1024x32bpp", 0x000000);
        gui_add_label(win, 12, 74, "GUI:  compositor + wm", 0x000000);

        gui_add_panel(win, 12, 100, 310, 2, GUI_COL_BORDER_DARK);

        gui_add_button(win, 100, 120, 120, 28, "Saludar", btn_hello_click);
        win->on_close = on_close_demo;
        gui_window_focus(win);
    }

    /* Second demo window */
    win = gui_window_create("Notas", 480, 120, 300, 200,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE);
    if (win) {
        gui_add_label(win, 12, 12, "Ventana de ejemplo", 0x000000);
        gui_add_label(win, 12, 34, "Arrastra la barra", 0x000000);
        gui_add_label(win, 12, 54, "de titulo para mover", 0x000000);
        gui_add_label(win, 12, 80, "Pulsa ESC para salir", 0x800000);

        gui_add_panel(win, 12, 110, 270, 40, 0xFFFFFF);
        gui_add_label(win, 20, 122, "Panel blanco", 0x404040);
        win->on_close = on_close_demo;
    }
}

/* ======== public API ======== */

void gui_init(void) {
    scr_w = (int)fb_width();
    scr_h = (int)fb_height();
    scr_pitch = fb_pitch();

    back_buffer = (uint32_t*)kmalloc(scr_w * scr_h * 4);
    if (!back_buffer) return;
    memset(back_buffer, 0, scr_w * scr_h * 4);

    mouse_x = scr_w / 2;
    mouse_y = scr_h / 2;
    drag_window_idx = -1;
    need_redraw = 1;
    gui_running = 0;

    /* hide the terminal mouse cursor while GUI is active */
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

void gui_run(void) {
    input_event_t ev;
    int desktop_h;
    gui_window_t* dragging_win = 0;

    gui_running = 1;
    need_redraw = 1;
    desktop_h = scr_h - GUI_TASKBAR_HEIGHT;

    while (gui_running) {
        /* process input events */
        while (input_poll_event(&ev)) {
            if (ev.device_type == INPUT_DEVICE_KEYBOARD) {
                /* ESC exits GUI */
                if (ev.type == INPUT_EVENT_CHAR && ev.character == 27) {
                    gui_running = 0;
                    break;
                }
                if (ev.type == INPUT_EVENT_CTRL_C) {
                    gui_running = 0;
                    break;
                }
                /* forward key to focused window */
                {
                    int i, count = gui_window_count();
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
                /* update mouse position (deltas match mouse_state convention) */
                mouse_x -= ev.delta_x;
                mouse_y -= ev.delta_y;
                if (mouse_x < 0) mouse_x = 0;
                if (mouse_y < 0) mouse_y = 0;
                if (mouse_x >= scr_w) mouse_x = scr_w - 1;
                if (mouse_y >= scr_h) mouse_y = scr_h - 1;

                need_redraw = 1;

                /* handle dragging */
                if (dragging_win) {
                    if (ev.buttons & 0x01) {
                        int nx = mouse_x - dragging_win->drag_off_x;
                        int ny = mouse_y - dragging_win->drag_off_y;
                        /* clamp so window stays partially visible */
                        if (nx < -(dragging_win->width - 40))
                            nx = -(dragging_win->width - 40);
                        if (ny < 0) ny = 0;
                        if (nx > scr_w - 40) nx = scr_w - 40;
                        if (ny > desktop_h - GUI_TITLEBAR_HEIGHT)
                            ny = desktop_h - GUI_TITLEBAR_HEIGHT;
                        gui_window_move(dragging_win, nx, ny);
                    } else {
                        dragging_win->dragging = 0;
                        dragging_win = 0;
                    }
                    continue;
                }

                /* left-button press */
                if (ev.buttons & 0x01) {
                    gui_window_t* w = hit_test_window(mouse_x, mouse_y);
                    if (w) {
                        gui_window_focus(w);

                        /* close button? */
                        if (hit_close_button(w, mouse_x, mouse_y)) {
                            if (w->on_close) w->on_close(w);
                            else gui_window_destroy(w);
                            continue;
                        }

                        /* title bar drag? */
                        if ((w->flags & GUI_WIN_DRAGGABLE) &&
                            hit_titlebar(w, mouse_x, mouse_y)) {
                            w->dragging = 1;
                            w->drag_off_x = mouse_x - w->x;
                            w->drag_off_y = mouse_y - w->y;
                            dragging_win = w;
                            continue;
                        }

                        /* widget click? */
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

        /* redraw scene */
        if (need_redraw) {
            need_redraw = 0;
            composite();
        }

        /* brief yield - avoids burning CPU in tight loop */
        __asm__ volatile("hlt");
    }

    /* cleanup */
    {
        int i, count = gui_window_count();
        for (i = count - 1; i >= 0; i--) {
            gui_window_t* w = gui_window_get(i);
            if (w) gui_window_destroy(w);
        }
    }
    if (back_buffer) {
        kfree(back_buffer);
        back_buffer = 0;
    }

    /* restore terminal display */
    fb_clear();
}
