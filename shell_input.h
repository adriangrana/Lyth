#ifndef SHELL_INPUT_H
#define SHELL_INPUT_H

#include "keyboard.h"

void shell_input_init(void);
void shell_input_handle_event(const keyboard_event_t* event);
void shell_input_print_history(void);
void shell_input_resume_prompt(void);

#endif
