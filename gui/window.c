#include "window.h"
#include "string.h"

static gui_window_t windows[GUI_MAX_WINDOWS];
static int window_used[GUI_MAX_WINDOWS];
static int next_window_id = 1;

gui_window_t* gui_window_create(const char* title, int x, int y,
                                int w, int h, uint32_t flags) {
    int i;
    gui_window_t* win;
    int len;

    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (!window_used[i]) {
            win = &windows[i];
            memset(win, 0, sizeof(*win));
            win->id = next_window_id++;
            win->x = x;
            win->y = y;
            win->width = w;
            win->height = h;
            win->flags = flags;
            win->z_order = i;
            len = strlen(title);
            if (len >= GUI_MAX_TITLE) len = GUI_MAX_TITLE - 1;
            memcpy(win->title, title, len);
            win->title[len] = '\0';
            window_used[i] = 1;
            return win;
        }
    }
    return 0;
}

void gui_window_destroy(gui_window_t* win) {
    int i;
    if (!win) return;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i] && &windows[i] == win) {
            window_used[i] = 0;
            break;
        }
    }
}

void gui_window_focus(gui_window_t* win) {
    int i, max_z;
    if (!win) return;
    max_z = 0;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i] && windows[i].z_order > max_z)
            max_z = windows[i].z_order;
        if (window_used[i])
            windows[i].flags &= ~GUI_WIN_FOCUSED;
    }
    win->flags |= GUI_WIN_FOCUSED;
    win->z_order = max_z + 1;
}

void gui_window_move(gui_window_t* win, int x, int y) {
    if (win) {
        win->x = x;
        win->y = y;
    }
}

int gui_window_content_x(gui_window_t* win) {
    return win->x + GUI_BORDER_WIDTH;
}

int gui_window_content_y(gui_window_t* win) {
    return win->y + GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH;
}

int gui_window_content_w(gui_window_t* win) {
    return win->width - GUI_BORDER_WIDTH * 2;
}

int gui_window_content_h(gui_window_t* win) {
    return win->height - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH * 2;
}

gui_widget_t* gui_add_label(gui_window_t* win, int x, int y,
                            const char* text, uint32_t fg) {
    gui_widget_t* w;
    int len;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    w = &win->widgets[win->widget_count++];
    memset(w, 0, sizeof(*w));
    w->type = GUI_WIDGET_LABEL;
    w->x = x;
    w->y = y;
    w->fg_color = fg;
    w->bg_color = GUI_COL_WINDOW_BG;
    len = strlen(text);
    if (len >= 47) len = 47;
    memcpy(w->text, text, len);
    w->text[len] = '\0';
    w->width = len * 8;
    w->height = 16;
    return w;
}

gui_widget_t* gui_add_button(gui_window_t* win, int x, int y,
                             int w, int h, const char* text,
                             void (*on_click)(gui_widget_t*)) {
    gui_widget_t* wi;
    int len;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    wi = &win->widgets[win->widget_count++];
    memset(wi, 0, sizeof(*wi));
    wi->type = GUI_WIDGET_BUTTON;
    wi->x = x;
    wi->y = y;
    wi->width = w;
    wi->height = h;
    wi->fg_color = GUI_COL_BTN_TEXT;
    wi->bg_color = GUI_COL_BTN_FACE;
    wi->on_click = on_click;
    len = strlen(text);
    if (len >= 47) len = 47;
    memcpy(wi->text, text, len);
    wi->text[len] = '\0';
    return wi;
}

gui_widget_t* gui_add_panel(gui_window_t* win, int x, int y,
                            int w, int h, uint32_t bg) {
    gui_widget_t* wi;
    if (!win || win->widget_count >= GUI_MAX_WIDGETS) return 0;
    wi = &win->widgets[win->widget_count++];
    memset(wi, 0, sizeof(*wi));
    wi->type = GUI_WIDGET_PANEL;
    wi->x = x;
    wi->y = y;
    wi->width = w;
    wi->height = h;
    wi->bg_color = bg;
    return wi;
}

int gui_window_count(void) {
    int count = 0, i;
    for (i = 0; i < GUI_MAX_WINDOWS; i++)
        if (window_used[i]) count++;
    return count;
}

gui_window_t* gui_window_get(int index) {
    int count = 0, i;
    for (i = 0; i < GUI_MAX_WINDOWS; i++) {
        if (window_used[i]) {
            if (count == index) return &windows[i];
            count++;
        }
    }
    return 0;
}
