#ifndef GUI_WIDGETS_H
#define GUI_WIDGETS_H

#include <stdint.h>

/* Forward declaration — full definition in window.h */
struct gui_window;

/* ============================================================
 *  Lyth OS Widget Kit
 *
 *  Centralized widget system with themed rendering, hover/focus
 *  tracking, and keyboard navigation. Widgets live inside
 *  gui_window_t.widgets[] and are drawn/dispatched by the
 *  compositor automatically.
 * ============================================================ */

/* ---- Widget types ---- */
typedef enum {
    WID_LABEL,
    WID_BUTTON,
    WID_CHECKBOX,
    WID_RADIO,
    WID_SLIDER,
    WID_TEXTINPUT,
    WID_PANEL,
    WID_SEPARATOR,
    WID_PROGRESS,
    WID_SWITCH,
    WID_TABS,
    WID_DROPDOWN,
    WID_LISTVIEW
} wid_type_t;

/* ---- Widget state flags ---- */
#define WID_ENABLED    (1 << 0)
#define WID_VISIBLE    (1 << 1)
#define WID_FOCUSED    (1 << 2)
#define WID_HOVERED    (1 << 3)
#define WID_PRESSED    (1 << 4)
#define WID_CHECKED    (1 << 5)

/* Default state for new widgets */
#define WID_DEFAULT    (WID_ENABLED | WID_VISIBLE)

/* ---- Callback types ---- */
struct wid_common;
typedef void (*wid_click_fn)(struct wid_common *w);
typedef void (*wid_change_fn)(struct wid_common *w, int value);

/* Max items for tabs/dropdown/listview */
#define WID_MAX_ITEMS   12
#define WID_ITEM_LEN    24

/* ---- Common widget struct (replaces gui_widget_t) ---- */
typedef struct wid_common {
    wid_type_t  type;
    uint16_t    state;     /* WID_ENABLED | WID_VISIBLE | ... */
    int16_t     x, y;
    int16_t     width, height;
    int16_t     id;        /* app-defined identifier */
    char        text[64];
    int         value;     /* checkbox: 0/1, slider: 0-max, radio: group_val */
    int         min_val, max_val; /* slider range */
    uint32_t    fg, bg;    /* 0 = use theme default */
    wid_click_fn  on_click;
    wid_change_fn on_change;
    /* tabs/dropdown/listview */
    char        items[WID_MAX_ITEMS][WID_ITEM_LEN];
    int16_t     item_count;
    int16_t     sel;       /* selected index */
    int16_t     scroll_off;/* listview scroll offset */
    int16_t     vis_rows;  /* listview visible rows */
} wid_t;

/* ============================================================
 *  Widget creation (returns pointer into win->widgets[])
 * ============================================================ */

/* Label: static text */
wid_t* wid_label(struct gui_window *win, int x, int y,
                 const char *text, uint32_t fg);

/* Button: clickable rectangle with text */
wid_t* wid_button(struct gui_window *win, int x, int y, int w, int h,
                  const char *text, wid_click_fn on_click);

/* Checkbox: toggleable check box with label */
wid_t* wid_checkbox(struct gui_window *win, int x, int y,
                    const char *label, int checked, wid_change_fn on_change);

/* Radio button: mutual exclusion within same group (value = group id) */
wid_t* wid_radio(struct gui_window *win, int x, int y,
                 const char *label, int group, int selected,
                 wid_change_fn on_change);

/* Slider: horizontal range [min..max] */
wid_t* wid_slider(struct gui_window *win, int x, int y, int w,
                  int min_val, int max_val, int cur_val,
                  wid_change_fn on_change);

/* Text input: single-line editable field */
wid_t* wid_textinput(struct gui_window *win, int x, int y, int w,
                     const char *placeholder);

/* Panel: colored rectangle (container/divider background) */
wid_t* wid_panel(struct gui_window *win, int x, int y, int w, int h,
                 uint32_t bg);

/* Separator: horizontal line */
wid_t* wid_separator(struct gui_window *win, int x, int y, int w);

/* Progress bar: 0..100 */
wid_t* wid_progress(struct gui_window *win, int x, int y, int w, int value);

/* Switch/toggle: on/off toggle */
wid_t* wid_switch(struct gui_window *win, int x, int y,
                  const char *label, int on, wid_change_fn on_change);

/* Tabs: horizontal tab bar. items = "|"-separated labels (e.g. "A|B|C") */
wid_t* wid_tabs(struct gui_window *win, int x, int y, int w,
                const char *items, int selected, wid_change_fn on_change);

/* Dropdown: click to open/close item list. items = "|"-separated. */
wid_t* wid_dropdown(struct gui_window *win, int x, int y, int w,
                    const char *items, int selected, wid_change_fn on_change);

/* ListView: scrollable list of items. items = "|"-separated, vis_rows = visible rows. */
wid_t* wid_listview(struct gui_window *win, int x, int y, int w,
                    int vis_rows, const char *items, int selected,
                    wid_change_fn on_change);

/* Add an item dynamically to a listview/dropdown. Returns 0 on success. */
int wid_add_item(wid_t *w, const char *item);

/* Clear all items from a listview/dropdown. */
void wid_clear_items(wid_t *w);

/* ============================================================
 *  Widget system (called by compositor)
 * ============================================================ */

/* Draw all widgets of a window onto its surface */
void wid_draw_all(struct gui_window *win);

/* Handle click at (rx, ry) in content coords. Returns 1 if consumed. */
int wid_handle_click(struct gui_window *win, int rx, int ry, int button);

/* Handle key event for focused widget. Returns 1 if consumed. */
int wid_handle_key(struct gui_window *win, int event_type, char key);

/* Update hover state from mouse coords (content-relative). */
void wid_update_hover(struct gui_window *win, int rx, int ry);

/* Clear hover state (mouse left window). */
void wid_clear_hover(struct gui_window *win);

/* Find widget by id. Returns NULL if not found. */
wid_t* wid_find(struct gui_window *win, int id);

/* Remove all widgets from a window. */
void wid_clear(struct gui_window *win);

#endif
