#ifndef GUI_AUTH_PROMPT_H
#define GUI_AUTH_PROMPT_H

/* Callback invoked after the auth prompt closes.
 * granted = 1 if the user authenticated successfully, 0 if cancelled. */
typedef void (*auth_callback_t)(int granted, void* userdata);

/* Show an authentication overlay that asks the current user
 * for their password.  When the user confirms or cancels,
 * cb is called.  reason is shown in the dialog. */
void auth_prompt_show(const char* reason, auth_callback_t cb, void* userdata);

#endif
