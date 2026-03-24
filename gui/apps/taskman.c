/*
 * Task Manager app — shows running processes with PID, state, priority.
 * Refreshes automatically every second.
 */

#include "taskman.h"
#include "compositor.h"
#include "window.h"
#include "font_psf.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "physmem.h"
#include "heap.h"

#define TM_MAX_TASKS 32

#define COL_TM_BG      0x1E1E2E
#define COL_TM_HEADER  0x313244
#define COL_TM_TEXT    0xCDD6F4
#define COL_TM_DIM     0x6C7086
#define COL_TM_RUNNING 0xA6E3A1
#define COL_TM_SLEEPING 0xF9E2AF
#define COL_TM_BLOCKED 0xF38BA8
#define COL_TM_READY   0x89B4FA
#define COL_TM_ACCENT  0x3B82F6

typedef struct {
    int is_open;
    unsigned int last_refresh;
} taskman_state_t;

static taskman_state_t tms;
static gui_window_t* tm_window;

static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static uint32_t state_color(task_state_t st) {
    switch (st) {
    case TASK_STATE_RUNNING:  return COL_TM_RUNNING;
    case TASK_STATE_READY:    return COL_TM_READY;
    case TASK_STATE_SLEEPING: return COL_TM_SLEEPING;
    case TASK_STATE_BLOCKED:  return COL_TM_BLOCKED;
    default:                  return COL_TM_DIM;
    }
}

static void tm_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    task_snapshot_t snaps[TM_MAX_TASKS];
    int count, i;
    int ox = GUI_BORDER_WIDTH + 8;
    int oy = GUI_TITLEBAR_HEIGHT + 4;
    int row_h = FONT_PSF_HEIGHT + 4;
    char buf[16];

    if (!s->pixels) return;

    gui_surface_clear(s, COL_TM_BG);

    /* title bar */
    gui_surface_fill(s, 0, 0, win->width, GUI_TITLEBAR_HEIGHT, 0x181825);
    {
        int cx = win->width - 20, cy = GUI_TITLEBAR_HEIGHT / 2, r;
        for (r = -5; r <= 5; r++) {
            int dx;
            for (dx = -5; dx <= 5; dx++) {
                if (r * r + dx * dx <= 25)
                    gui_surface_putpixel(s, cx + dx, cy + r, 0xF38BA8);
            }
        }
    }
    gui_surface_draw_string(s, 10, (GUI_TITLEBAR_HEIGHT - FONT_PSF_HEIGHT) / 2,
                            win->title, 0xCDD6F4, 0, 0);
    gui_surface_hline(s, 0, GUI_TITLEBAR_HEIGHT - 1, win->width, 0x313244);

    /* memory bar */
    {
        unsigned int total = physmem_total_bytes() / 1024;
        unsigned int used = (physmem_total_bytes() - physmem_free_bytes()) / 1024;
        int bar_w = win->width - ox * 2;
        int fill_w = total > 0 ? (int)((unsigned long)used * (unsigned long)bar_w / total) : 0;

        gui_surface_draw_string(s, ox, oy, "Memory:", COL_TM_TEXT, 0, 0);
        int_to_str(used, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 64, oy, buf, COL_TM_TEXT, 0, 0);
        gui_surface_draw_string(s, ox + 64 + (int)strlen(buf) * FONT_PSF_WIDTH, oy,
                                " / ", COL_TM_DIM, 0, 0);
        int_to_str(total, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 88 + (int)strlen(buf) * FONT_PSF_WIDTH, oy,
                                buf, COL_TM_TEXT, 0, 0);
        gui_surface_draw_string(s, ox + 88 + (int)strlen(buf) * FONT_PSF_WIDTH * 2, oy,
                                " KB", COL_TM_DIM, 0, 0);

        oy += row_h + 2;
        gui_surface_fill(s, ox, oy, bar_w, 8, COL_TM_HEADER);
        if (fill_w > 0) gui_surface_fill(s, ox, oy, fill_w, 8, COL_TM_ACCENT);
        oy += 14;
    }

    /* header */
    gui_surface_fill(s, ox, oy, win->width - ox * 2, row_h, COL_TM_HEADER);
    gui_surface_draw_string(s, ox + 4, oy + 2, "PID", COL_TM_TEXT, 0, 0);
    gui_surface_draw_string(s, ox + 48, oy + 2, "State", COL_TM_TEXT, 0, 0);
    gui_surface_draw_string(s, ox + 148, oy + 2, "Priority", COL_TM_TEXT, 0, 0);
    gui_surface_draw_string(s, ox + 260, oy + 2, "Name", COL_TM_TEXT, 0, 0);
    oy += row_h;

    /* processes */
    count = task_list(snaps, TM_MAX_TASKS);
    for (i = 0; i < count; i++) {
        if (oy + row_h > win->height - 4) break;

        int_to_str(snaps[i].id, buf, sizeof(buf));
        gui_surface_draw_string(s, ox + 4, oy + 2, buf, COL_TM_TEXT, 0, 0);

        const char* st = task_state_name(snaps[i].state);
        gui_surface_draw_string(s, ox + 48, oy + 2, st, state_color(snaps[i].state), 0, 0);

        const char* pr = task_priority_name(snaps[i].priority);
        gui_surface_draw_string(s, ox + 148, oy + 2, pr, COL_TM_DIM, 0, 0);

        gui_surface_draw_string(s, ox + 260, oy + 2, snaps[i].name, COL_TM_TEXT, 0, 0);

        oy += row_h;
        gui_surface_hline(s, ox, oy - 1, win->width - ox * 2, 0x25253A);
    }

    tms.last_refresh = timer_get_uptime_ms();
}

static void tm_on_key(gui_window_t* win, int event_type, char key) {
    (void)event_type;
    if (key == 'r' || key == 'R') {
        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

static void tm_on_close(gui_window_t* win) {
    tms.is_open = 0;
    tm_window = 0;
    gui_dirty_add(win->x - 6, win->y - 6, win->width + 12, win->height + 12);
    gui_window_destroy(win);
}

void taskman_app_open(void) {
    if (tms.is_open && tm_window) {
        gui_window_focus(tm_window);
        gui_dirty_add(tm_window->x, tm_window->y,
                      tm_window->width, tm_window->height);
        return;
    }

    tm_window = gui_window_create("Task Manager", 200, 60, 420, 400,
        GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE | GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED);
    if (!tm_window) return;

    tm_window->on_paint = tm_paint;
    tm_window->on_key = tm_on_key;
    tm_window->on_close = tm_on_close;

    tms.is_open = 1;
    tms.last_refresh = 0;
    gui_window_focus(tm_window);
    gui_dirty_add(tm_window->x, tm_window->y,
                  tm_window->width, tm_window->height);
}
