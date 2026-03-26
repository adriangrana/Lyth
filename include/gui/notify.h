#ifndef GUI_NOTIFY_H
#define GUI_NOTIFY_H

/* Push a notification toast. It will appear near the top-right corner
 * and auto-dismiss after a few seconds.  title and body are copied
 * internally.  Safe to call from any context. */
void notify_push(const char* title, const char* body);

/* Called by the compositor each frame to paint active toasts. */
void notify_paint(void* surface, int screen_w);

/* Tick — call periodically to expire old toasts. */
void notify_tick(void);

/* How many active notifications remain (0 = none visible). */
int notify_count(void);

/* ---- On-Screen Display (centered indicator) ---- */
/* Show a centered OSD bar with label and percentage level (0–100).
 * Auto-dismisses after ~1.5s.  icon is a single ASCII char. */
void osd_show(char icon, const char *label, int level);

/* Paint the OSD into the given surface if active. */
void osd_paint(void *surface, int screen_w, int screen_h);

/* Tick — call to expire OSD. */
void osd_tick(void);

/* Returns 1 if OSD is currently visible. */
int osd_active(void);

#endif
