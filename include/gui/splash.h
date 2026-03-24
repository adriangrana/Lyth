#ifndef GUI_SPLASH_H
#define GUI_SPLASH_H

/* Show the boot splash screen (direct framebuffer rendering). */
void splash_show(void);

/* Update the progress message shown on the splash screen. */
void splash_set_message(const char* msg);

/* Update the progress bar (0-100). */
void splash_set_progress(int percent);

/* Hide the splash screen and clear the framebuffer. */
void splash_hide(void);

#endif /* GUI_SPLASH_H */
