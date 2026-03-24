#ifndef GUI_LOGIN_H
#define GUI_LOGIN_H

/* Run the login manager inside the compositor.
 * Creates the login window and enters the GUI event loop.
 * Returns the uid of the authenticated user, or (unsigned)-1 on failure. */
unsigned int login_manager_run(void);

/* Request return to login screen (called from desktop on logout). */
void login_manager_request_logout(void);

/* Returns 1 while the login screen is the active GUI mode. */
int login_manager_is_active(void);

#endif /* GUI_LOGIN_H */
