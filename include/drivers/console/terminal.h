#ifndef TERMINAL_H
#define TERMINAL_H

void terminal_init(void);
void terminal_clear(void);
void terminal_put_char(char c);
void terminal_print(const char* str);
void terminal_print_line(const char* str);
void terminal_print_uint(unsigned int value);
void terminal_print_hex(unsigned int value);
void terminal_backspace(void);
void terminal_set_color(unsigned char color);
void terminal_put_char_with_color(char c, unsigned char color);
void terminal_set_overwrite_mode(int enabled);
int terminal_overwrite_mode(void);
void terminal_capture_begin(char* buffer, unsigned int buffer_size);
unsigned int terminal_capture_end(void);
int terminal_capture_begin_dynamic(unsigned int initial_size);
char* terminal_capture_end_dynamic(unsigned int* length);
void terminal_get_cursor(int* row, int* col);
void terminal_set_cursor(int row, int col);
void terminal_update_cursor(void);
int terminal_rows(void);
int terminal_columns(void);

#endif