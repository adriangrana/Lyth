#ifndef GUI_DESKTOP_H
#define GUI_DESKTOP_H

#include "window.h"

/* Initialise desktop (gradient, taskbar, start menu state) */
void desktop_init(int scr_w, int scr_h);
void desktop_shutdown(void);
void desktop_resize(int new_w, int new_h);

/* Paint a region of the desktop background into a surface.
 * Called by compositor for each dirty rect before overlaying windows. */
void desktop_paint_region(gui_surface_t* dst, int x0, int y0, int x1, int y1);

/* Called once per second to update clock display */
void desktop_on_tick(void);

/* Called every frame to advance launcher animation */
void desktop_anim_tick(void);

/* Update hover state for dock/taskbar (called on mouse move) */
void desktop_update_hover(int mx, int my, int buttons);

/* Input handlers - return 1 if consumed */
int desktop_handle_click(int mx, int my, int button);
int desktop_handle_key(int event_type, char key);

/* Height of the top menu bar area */
int desktop_get_menubar_height(void);

/* Access to the pre-rendered desktop surface (gradient + taskbar) */
gui_surface_t* desktop_get_surface(void);

/* Paint overlays that must appear on top of all windows (launcher) */
void desktop_paint_overlays(gui_surface_t* dst, int x0, int y0, int x1, int y1);

/* Returns 1 if the start menu or other top-level overlay is open */
int desktop_is_overlay_open(void);

/* Close all top-level overlays (start menu) */
void desktop_close_overlays(void);

/* Wallpaper catalogue API — used by settings app */
int         desktop_wallpaper_count(void);
int         desktop_wallpaper_selected(void);
const char* desktop_wallpaper_name(int idx);
int         desktop_wallpaper_is_image(int idx);   /* 1 = PNG, 0 = solid */
uint32_t    desktop_wallpaper_solid_col(int idx);
const uint32_t* desktop_wallpaper_pixels(int idx, int *out_w, int *out_h);
void        desktop_set_wallpaper(int idx);

/* Invalidate the desktop cache so the taskbar window list gets redrawn */
void desktop_invalidate_taskbar(void);

/* Invalidate entire desktop (e.g. theme change) */
void desktop_invalidate_all(void);

/* Taskbar auto-hide API */
int  desktop_taskbar_autohide_get(void);
void desktop_taskbar_autohide_set(int on);

#endif
