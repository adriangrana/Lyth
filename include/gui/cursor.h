#ifndef GUI_CURSOR_H
#define GUI_CURSOR_H

#include "window.h"

void cursor_init(int screen_w, int screen_h);
void cursor_resize(int new_w, int new_h);
void cursor_set_pos(int x, int y);
void cursor_get_pos(int* x, int* y);

/* Draw cursor onto backbuffer (saves underlying pixels) */
void cursor_draw(gui_surface_t* bb);
/* Erase cursor from backbuffer (restores saved pixels) */
void cursor_erase(gui_surface_t* bb);

/* Add dirty rects for old and new cursor position */
void cursor_invalidate_old(void);
void cursor_invalidate_new(void);

#endif
