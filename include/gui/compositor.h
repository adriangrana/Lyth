#ifndef GUI_COMPOSITOR_H
#define GUI_COMPOSITOR_H

void gui_init(void);
void gui_run(void);
void gui_stop(void);
void gui_request_redraw(void);
int  gui_is_active(void);

#endif
