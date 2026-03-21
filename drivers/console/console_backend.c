#include "console_backend.h"
#include "fbconsole.h"
#include "terminal.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile unsigned short* const VGA = (volatile unsigned short*)0xB8000;

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int vga_rows(void) {
    return VGA_HEIGHT;
}

static int vga_columns(void) {
    return VGA_WIDTH;
}

static int vga_index(int row, int col) {
    return row * VGA_WIDTH + col;
}

static void vga_clear(unsigned char color) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA[i] = ((unsigned short)color << 8) | ' ';
    }
}

static void vga_scroll(unsigned char color) {
    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA[vga_index(row - 1, col)] = VGA[vga_index(row, col)];
        }
    }

    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA[vga_index(VGA_HEIGHT - 1, col)] = ((unsigned short)color << 8) | ' ';
    }
}

static void vga_put_cell(int row, int col, unsigned int glyph, unsigned char color) {
    if (row < 0 || row >= VGA_HEIGHT || col < 0 || col >= VGA_WIDTH) {
        return;
    }

    VGA[vga_index(row, col)] = ((unsigned short)color << 8) | (unsigned char)glyph;
}

static void vga_show_cursor(int row, int col, unsigned char color) {
    unsigned short pos;
    unsigned char start_scanline;
    unsigned char end_scanline;

    (void)color;

    if (row < 0) {
        row = 0;
    }

    if (col < 0) {
        col = 0;
    }

    if (row >= VGA_HEIGHT) {
        row = VGA_HEIGHT - 1;
    }

    if (col >= VGA_WIDTH) {
        col = VGA_WIDTH - 1;
    }

    pos = (unsigned short)(row * VGA_WIDTH + col);

    if (terminal_overwrite_mode()) {
        start_scanline = 0;
        end_scanline = 15;
    } else {
        start_scanline = 13;
        end_scanline = 15;
    }

    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
    outb(0x3D4, 0x0A);
    outb(0x3D5, start_scanline);
    outb(0x3D4, 0x0B);
    outb(0x3D5, end_scanline);
}

static void fb_clear_with_color(unsigned char color) {
    (void)color;
    fb_clear();
}

static void fb_show_cursor(int row, int col, unsigned char color) {
    fb_draw_cursor_at(row, col, color);
}

static const console_backend_t vga_backend = {
    vga_rows,
    vga_columns,
    vga_clear,
    vga_scroll,
    vga_put_cell,
    vga_show_cursor,
    0
};

static const console_backend_t framebuffer_backend = {
    fb_rows,
    fb_columns,
    fb_clear_with_color,
    fb_scroll,
    fb_put_glyph_at,
    fb_show_cursor,
    1
};

const console_backend_t* console_backend_current(void) {
    return fb_active() ? &framebuffer_backend : &vga_backend;
}