#include "window.h"
#include "string.h"

#define GUI_TEXT_CHAR_W 8
#define GUI_TEXT_CHAR_H 16

static gui_window_t windows[GUI_MAX_WINDOWS];
static int window_used[GUI_MAX_WINDOWS];
static int next_window_id = 1;

static int label_wrap_width(gui_window_t* win, gui_widget_t* label) {
    int i;
    int wrap_width = 0;

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
            if (available > 0 && (wrap_width == 0 || available < wrap_width)) {
                wrap_width = available;
            }
        }
    }

    return wrap_width;
}

static void relayout_panel_children(gui_window_t* win, gui_widget_t* panel) {
    int i;
    int next_y = panel->y + 14;
    int panel_bottom = panel->y + panel->height;

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* child = &win->widgets[i];

        if (child == panel) {
            continue;
        }

        if (child->x < panel->x || child->x >= panel->x + panel->width) {
            continue;
        }

        if (child->y < panel->y || child->y >= panel_bottom) {
            continue;
        }

        if (child->y < next_y) {
            child->y = next_y;
        }

        next_y = child->y + child->height + 6;
    }

    if (next_y + 8 > panel_bottom) {
        panel->height = (next_y + 8) - panel->y;
    }
}

static void measure_wrapped_text(const char* text, int max_width,
                                 int* out_width, int* out_height) {
    int max_chars;
    int max_line_chars = 0;
    int line_count = 0;
    const char* p = text;

    if (!text || !*text) {
        *out_width = 0;
        *out_height = GUI_TEXT_CHAR_H;
        return;
    }

    max_chars = max_width / GUI_TEXT_CHAR_W;
    if (max_chars <= 0) {
        max_chars = 1;
    }

    while (*p) {
        int line_chars = 0;

        while (*p == ' ') {
            p++;
        }

        while (*p) {
            const char* word = p;
            int word_len = 0;

            while (word[word_len] && word[word_len] != ' ') {
                word_len++;
            }

            if (line_chars == 0) {
                if (word_len <= max_chars) {
                    line_chars = word_len;
                    p += word_len;
                } else {
                    line_chars = max_chars;
                    p += max_chars;
                }
            } else if (line_chars + 1 + word_len <= max_chars) {
                line_chars += 1 + word_len;
                p += word_len;
            } else {
                break;
            }

            while (*p == ' ') {
                p++;
            }
        }

        if (line_chars > max_line_chars) {
            max_line_chars = line_chars;
        }
        line_count++;
    }

    if (line_count <= 0) {
        line_count = 1;
    }

    *out_width = max_line_chars * GUI_TEXT_CHAR_W;
    *out_height = line_count * GUI_TEXT_CHAR_H;
}

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

void gui_window_fit_to_content(gui_window_t* win, int padding_right,
                               int padding_bottom, int min_width,
                               int min_height) {
    int i;
    int content_w = 0;
    int content_h = 0;
    int title_w;

    if (!win) return;

    if (padding_right < 0) padding_right = 0;
    if (padding_bottom < 0) padding_bottom = 0;

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        if (w->type == GUI_WIDGET_LABEL) {
            int wrap_width = label_wrap_width(win, w);
            int text_w;
            int text_h;

            if (wrap_width > 0) {
                measure_wrapped_text(w->text, wrap_width, &text_w, &text_h);
                w->width = wrap_width;
                w->height = text_h;
            } else {
                w->width = (int)strlen(w->text) * GUI_TEXT_CHAR_W;
                w->height = GUI_TEXT_CHAR_H;
            }
        }
    }

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        if (w->type == GUI_WIDGET_PANEL) {
            relayout_panel_children(win, w);
        }
    }

    for (i = 0; i < win->widget_count; i++) {
        gui_widget_t* w = &win->widgets[i];
        int right = w->x + w->width;
        int bottom = w->y + w->height;

        if (right > content_w) content_w = right;
        if (bottom > content_h) content_h = bottom;
    }

    content_w += padding_right;
    content_h += padding_bottom;

    title_w = (int)strlen(win->title) * 8 + 112;

    if (content_w + GUI_BORDER_WIDTH * 2 < title_w) {
        content_w = title_w - GUI_BORDER_WIDTH * 2;
    }

    if (content_w + GUI_BORDER_WIDTH * 2 < min_width) {
        content_w = min_width - GUI_BORDER_WIDTH * 2;
    }

    if (content_h + GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH * 2 < min_height) {
        content_h = min_height - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH * 2;
    }

    win->width = content_w + GUI_BORDER_WIDTH * 2;
    win->height = content_h + GUI_TITLEBAR_HEIGHT + GUI_BORDER_WIDTH * 2;
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
    w->bg_color = 0xF5F7FA;

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

    wi->fg_color = 0xFFFFFF;
    wi->bg_color = 0x2D6CDF;
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