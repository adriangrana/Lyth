#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdint.h>
#include "theme.h"
#include "widgets.h"

/* ---- layout constants (sourced from theme.h) ---- */
#define GUI_TITLEBAR_HEIGHT  THEME_TITLEBAR_H
#define GUI_BORDER_WIDTH     THEME_BORDER_W
#define GUI_MAX_WINDOWS      32
#define GUI_MAX_TITLE        64
#define GUI_MAX_WIDGETS      48
#define GUI_TASKBAR_HEIGHT   THEME_TASKBAR_H
#define GUI_DOCK_HEIGHT      THEME_DOCK_H
#define GUI_FONT_W           THEME_FONT_W
#define GUI_FONT_H           THEME_FONT_H

/* ---- window flags ---- */
#define GUI_WIN_VISIBLE    (1 << 0)
#define GUI_WIN_FOCUSED    (1 << 1)
#define GUI_WIN_CLOSEABLE  (1 << 2)
#define GUI_WIN_DRAGGABLE  (1 << 3)
#define GUI_WIN_MINIMIZED  (1 << 4)
#define GUI_WIN_RESIZABLE  (1 << 5)
#define GUI_WIN_NO_DECOR   (1 << 6)
#define GUI_WIN_STICKY     (1 << 7)  /* visible on all workspaces */

#define GUI_MAX_WORKSPACES 4

/* resize edge identifiers */
#define GUI_RESIZE_NONE   0
#define GUI_RESIZE_RIGHT  1
#define GUI_RESIZE_BOTTOM 2
#define GUI_RESIZE_BR     3  /* bottom-right corner */
#define GUI_RESIZE_GRAB   6  /* grab zone width in pixels */
#define GUI_RESIZE_MIN_W  120
#define GUI_RESIZE_MIN_H  80

/* ---- surface: pixel buffer ---- */
typedef struct {
    uint32_t* pixels;
    int width;
    int height;
    int stride;
    uint32_t alloc_phys;
    uint32_t alloc_size;
} gui_surface_t;

/* ---- rectangle ---- */
typedef struct {
    int x, y, w, h;
} gui_rect_t;

struct gui_window;
typedef void (*gui_paint_fn)(struct gui_window*);
typedef void (*gui_close_fn)(struct gui_window*);
typedef void (*gui_key_fn)(struct gui_window*, int event_type, char key);
typedef void (*gui_click_fn)(struct gui_window*, int x, int y, int button);
typedef void (*gui_dblclick_fn)(struct gui_window*, int x, int y);

typedef struct gui_window {
    int id;
    int x, y;
    int width, height;
    char title[GUI_MAX_TITLE];
    uint32_t flags;

    int dragging;
    int drag_off_x, drag_off_y;

    int resizing;      /* non-zero = resize edge being dragged */
    int resize_orig_w, resize_orig_h;  /* size at start of resize */
    int resize_orig_mx, resize_orig_my; /* mouse at start of resize */

    /* pre-snap geometry (restored on un-snap) */
    int snap_restore_x, snap_restore_y;
    int snap_restore_w, snap_restore_h;
    int snapped;  /* 0=none, 1=left, 2=right, 3=full */

    gui_surface_t surface;
    int needs_redraw;

    wid_t widgets[GUI_MAX_WIDGETS];
    int widget_count;

    gui_paint_fn on_paint;
    gui_close_fn on_close;
    gui_key_fn   on_key;
    gui_click_fn on_click;
    gui_dblclick_fn on_dblclick;

    int z_order;
    uint8_t alpha;     /* per-window opacity: 255=opaque, 0=invisible */
    uint8_t anim_alpha_target;  /* fade target: 255=fade in, 0=fade out */
    uint8_t anim_closing;       /* 1 = window is fading out before destroy */
    uint8_t anim_minimizing;    /* 1 = window is fading out before minimize */
    uint8_t anim_alpha_start;   /* alpha when animation started */
    unsigned int anim_start_ms; /* timestamp when current anim began */
    unsigned int anim_dur_ms;   /* duration of current animation (ms) */
    unsigned int redraw_count;  /* debug: total redraws for this window */
    int workspace;              /* workspace index (0-based), -1 = sticky */
    void* app_data;
} gui_window_t;

/* ---- window API ---- */
gui_window_t* gui_window_create(const char* title, int x, int y,
                                int w, int h, uint32_t flags);
void gui_window_destroy(gui_window_t* win);
void gui_window_close_animated(gui_window_t* win);
void gui_window_anim_start(gui_window_t* win, uint8_t target, unsigned int dur_ms);
void gui_window_anim_tick(void);
void gui_window_focus(gui_window_t* win);
void gui_window_move(gui_window_t* win, int x, int y);
void gui_window_resize(gui_window_t* win, int new_w, int new_h);
void gui_window_invalidate(gui_window_t* win);
void gui_window_draw_decorations(gui_window_t* win);
int  gui_window_content_x(gui_window_t* win);
int  gui_window_content_y(gui_window_t* win);
int  gui_window_content_w(gui_window_t* win);
int  gui_window_content_h(gui_window_t* win);

int  gui_window_count(void);
gui_window_t* gui_window_get(int index);

/* ---- surface drawing ops (fast, no clipping beyond bounds) ---- */
void gui_surface_alloc(gui_surface_t* s, int w, int h);
void gui_surface_free(gui_surface_t* s);
void gui_surface_clear(gui_surface_t* s, uint32_t c);
void gui_surface_fill(gui_surface_t* s, int x, int y, int w, int h, uint32_t c);
void gui_surface_hline(gui_surface_t* s, int x, int y, int w, uint32_t c);
void gui_surface_putpixel(gui_surface_t* s, int x, int y, uint32_t c);
void gui_surface_draw_char(gui_surface_t* s, int x, int y, unsigned char ch,
                           uint32_t fg, uint32_t bg, int draw_bg);
void gui_surface_draw_string(gui_surface_t* s, int x, int y, const char* str,
                             uint32_t fg, uint32_t bg, int draw_bg);
void gui_surface_draw_string_n(gui_surface_t* s, int x, int y, const char* str,
                               int max_chars, uint32_t fg, uint32_t bg, int draw_bg);

/* Scaled text (2x nearest-neighbor) for headings/titles */
void gui_surface_draw_char_2x(gui_surface_t* s, int x, int y, unsigned char ch,
                               uint32_t fg, uint32_t bg, int draw_bg);
void gui_surface_draw_string_2x(gui_surface_t* s, int x, int y, const char* str,
                                 uint32_t fg, uint32_t bg, int draw_bg);

/* ---- workspace API ---- */
int  gui_workspace_current(void);
void gui_workspace_switch(int ws);
int  gui_window_on_current_ws(gui_window_t *w);

#endif
