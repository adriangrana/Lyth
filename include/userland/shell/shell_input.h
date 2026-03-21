#ifndef SHELL_INPUT_H
#define SHELL_INPUT_H

#include "input.h"

void shell_input_init(void);
void shell_input_handle_event(const input_event_t* event);
void shell_input_print_history(void);
void shell_input_resume_prompt(void);
void shell_input_set_theme(unsigned char bracket_color, unsigned char label_color, unsigned char suffix_color, unsigned char selection_color);

#endif
