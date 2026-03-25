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

#endif
