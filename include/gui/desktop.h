#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "window.h"

/* Initialise desktop (gradient, taskbar, start menu state) */
void desktop_init(int scr_w, int scr_h);
void desktop_shutdown(void);

/* Paint a region of the desktop background into a surface.
 * Called by compositor for each dirty rect before overlaying windows. */
void desktop_paint_region(gui_surface_t* dst, int x0, int y0, int x1, int y1);

/* Called once per second to update clock display */
void desktop_on_tick(void);

/* Input handlers - return 1 if consumed */
int desktop_handle_click(int mx, int my);
int desktop_handle_key(int event_type, char key);

/* Height of the top menu bar area */
int desktop_get_menubar_height(void);

/* Access to the pre-rendered desktop surface (gradient + taskbar) */
gui_surface_t* desktop_get_surface(void);

#endif
