#ifndef GUI_COMPOSITOR_H
#define GUI_COMPOSITOR_H

#include <stdint.h>
#include "window.h"

void gui_init(void);
void gui_run(void);
void gui_stop(void);
void gui_request_redraw(void);
int  gui_is_active(void);

int gui_screen_width(void);
int gui_screen_height(void);
gui_surface_t* gui_get_backbuffer(void);

/* hot resolution change */
int gui_resize_screen(int new_w, int new_h);

/* dirty rect management */
#define GUI_MAX_DIRTY 64

typedef struct {
    int x, y, w, h;
} gui_dirty_rect_t;

void gui_dirty_add(int x, int y, int w, int h);
void gui_dirty_add_rect(const gui_dirty_rect_t* r);
void gui_dirty_screen(void);

/* debug metrics */
typedef struct {
    unsigned int fps;              /* actual presents per second */
    unsigned int frame_time_us;    /* last frame total (compose+present) */
    unsigned int frame_time_avg;   /* rolling average frame time µs */
    unsigned int frame_time_max;   /* max frame time in last second */
    unsigned int render_us;
    unsigned int compose_us;
    unsigned int present_us;
    unsigned int dirty_count;
    unsigned int pixels_copied;
    unsigned int drag_active;      /* 1 if currently dragging */
    unsigned int coalesced_moves;  /* mouse moves coalesced this frame */
    /* drag instrumentation */
    unsigned int drag_mouse_x;     /* cursor x during drag */
    unsigned int drag_mouse_y;     /* cursor y during drag */
    unsigned int drag_win_x;       /* window x after last move */
    unsigned int drag_win_y;       /* window y after last move */
    unsigned int drag_move_count;  /* window_move() calls this second */
    unsigned int drag_pending_count; /* loop iters with drag pending */
} gui_metrics_t;

void gui_get_metrics(gui_metrics_t* out);

#endif
