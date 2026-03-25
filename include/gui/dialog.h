#ifndef GUI_DIALOG_H
#define GUI_DIALOG_H

/* Callback invoked when the user selects a file or cancels.
 * path is NULL if the user cancelled. */
typedef void (*dialog_callback_t)(const char* path, void* userdata);

/* Open a file-open dialog.  When the user picks a file or cancels,
 * cb is called.  start_dir may be NULL to use "/". */
void dialog_open_file(const char* start_dir, dialog_callback_t cb, void* userdata);

/* Open a file-save dialog.  default_name can be NULL.
 * cb is called with the chosen full path or NULL on cancel. */
void dialog_save_file(const char* start_dir, const char* default_name,
                      dialog_callback_t cb, void* userdata);

#endif
