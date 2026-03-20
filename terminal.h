#ifndef TERMINAL_H
#define TERMINAL_H

void terminal_init(void);
void terminal_clear(void);
void terminal_put_char(char c);
void terminal_print(const char* str);
void terminal_print_line(const char* str);
void terminal_backspace(void);
void terminal_set_color(unsigned char color);

#endif