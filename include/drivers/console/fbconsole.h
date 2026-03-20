#ifndef FBCONSOLE_H
#define FBCONSOLE_H

#include <stdint.h>
#include "multiboot.h"

typedef struct {
    void* addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t bpp;
    uint8_t type;
} framebuffer_info_t;

int fb_init(multiboot_info_t* mbi);
int fb_active(void);
int fb_rows(void);
int fb_columns(void);
void fb_clear(void);
void fb_scroll(unsigned char color);
void fb_put_char_at(int row, int col, char c, unsigned char color);
void fb_draw_cursor_at(int row, int col, unsigned char color);

uint32_t fb_width(void);
uint32_t fb_height(void);
uint32_t fb_pitch(void);
uint8_t fb_bpp(void);
uint8_t fb_type(void);

#endif