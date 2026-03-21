#ifndef CONSOLE_BACKEND_H
#define CONSOLE_BACKEND_H

typedef struct {
    int (*rows)(void);
    int (*columns)(void);
    void (*clear)(unsigned char color);
    void (*scroll)(unsigned char color);
    void (*put_cell)(int row, int col, unsigned int glyph, unsigned char color);
    void (*show_cursor)(int row, int col, unsigned char color);
    int software_cursor;
} console_backend_t;

const console_backend_t* console_backend_current(void);

#endif