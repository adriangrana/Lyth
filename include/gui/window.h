#ifndef GUI_WINDOW_H
#define GUI_WINDOW_H

#include <stdint.h>

/* ---- layout constants ---- */
#define GUI_TITLEBAR_HEIGHT  20
#define GUI_BORDER_WIDTH      2
#define GUI_MAX_WINDOWS      16
#define GUI_MAX_TITLE        64
#define GUI_MAX_WIDGETS      32
#define GUI_TASKBAR_HEIGHT   28
#define GUI_CLOSE_BTN_SIZE   14

/* ---- colours (0xRRGGBB) ---- */
#define GUI_COL_DESKTOP      0x008080
#define GUI_COL_TITLEBAR     0x000080
#define GUI_COL_TITLEBAR_IA  0x808080
#define GUI_COL_TITLE_TEXT   0xFFFFFF
#define GUI_COL_WINDOW_BG    0xC0C0C0
#define GUI_COL_BORDER       0x000000
#define GUI_COL_BORDER_LIGHT 0xFFFFFF
#define GUI_COL_BORDER_DARK  0x808080
#define GUI_COL_BTN_FACE     0xC0C0C0
#define GUI_COL_BTN_TEXT     0x000000
#define GUI_COL_BTN_HOVER    0xD4D0C8
#define GUI_COL_CLOSE_BG     0xC04040
#define GUI_COL_CLOSE_FG     0xFFFFFF
#define GUI_COL_TASKBAR      0xC0C0C0
#define GUI_COL_TASKBAR_TEXT 0x000000
#define GUI_COL_START_BTN    0x008000
#define GUI_COL_START_TEXT   0xFFFFFF

/* ---- window flags ---- */
#define GUI_WIN_VISIBLE    (1 << 0)
#define GUI_WIN_FOCUSED    (1 << 1)
#define GUI_WIN_CLOSEABLE  (1 << 2)
#define GUI_WIN_DRAGGABLE  (1 << 3)
#define GUI_WIN_MINIMIZED  (1 << 4)

/* ---- widget types ---- */
typedef enum {
    GUI_WIDGET_LABEL,
    GUI_WIDGET_BUTTON,
    GUI_WIDGET_PANEL
} gui_widget_type_t;

typedef struct gui_widget {
    gui_widget_type_t type;
    int x, y;           /* relative to window content area */
    int width, height;
    char text[48];
    uint32_t fg_color;
    uint32_t bg_color;
    void (*on_click)(struct gui_widget* w);
    int id;
} gui_widget_t;

struct gui_window;
typedef void (*gui_paint_fn)(struct gui_window*);
typedef void (*gui_close_fn)(struct gui_window*);
typedef void (*gui_key_fn)(struct gui_window*, char key);
typedef void (*gui_click_fn)(struct gui_window*, int x, int y, int button);

typedef struct gui_window {
    int id;
    int x, y;
    int width, height;       /* total size including decorations */
    char title[GUI_MAX_TITLE];
    uint32_t flags;

    /* drag state */
    int dragging;
    int drag_off_x, drag_off_y;

    /* widgets */
    gui_widget_t widgets[GUI_MAX_WIDGETS];
    int widget_count;

    /* callbacks */
    gui_paint_fn on_paint;
    gui_close_fn on_close;
    gui_key_fn   on_key;
    gui_click_fn on_click;

    /* z-order (higher = on top) */
    int z_order;
} gui_window_t;

/* ---- window API ---- */
gui_window_t* gui_window_create(const char* title, int x, int y,
                                int w, int h, uint32_t flags);
void gui_window_destroy(gui_window_t* win);
void gui_window_focus(gui_window_t* win);
void gui_window_move(gui_window_t* win, int x, int y);
int  gui_window_content_x(gui_window_t* win);
int  gui_window_content_y(gui_window_t* win);
int  gui_window_content_w(gui_window_t* win);
int  gui_window_content_h(gui_window_t* win);

/* widget helpers */
gui_widget_t* gui_add_label(gui_window_t* win, int x, int y,
                            const char* text, uint32_t fg);
gui_widget_t* gui_add_button(gui_window_t* win, int x, int y,
                             int w, int h, const char* text,
                             void (*on_click)(gui_widget_t*));
gui_widget_t* gui_add_panel(gui_window_t* win, int x, int y,
                            int w, int h, uint32_t bg);

/* internal: iterate windows in z-order (used by compositor) */
int  gui_window_count(void);
gui_window_t* gui_window_get(int index);

#endif
