#include "terminal.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile unsigned short* const VGA = (volatile unsigned short*) 0xB8000;
static unsigned char terminal_color = 0x0F;

static int cursor_row = 0;
static int cursor_col = 0;

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int vga_index(int row, int col) {
    return row * VGA_WIDTH + col;
}

static void update_cursor(void) {
    unsigned short pos = (unsigned short)(cursor_row * VGA_WIDTH + cursor_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void scroll_if_needed(void) {
    if (cursor_row < VGA_HEIGHT) {
        return;
    }

    for (int row = 1; row < VGA_HEIGHT; row++) {
        for (int col = 0; col < VGA_WIDTH; col++) {
            VGA[vga_index(row - 1, col)] = VGA[vga_index(row, col)];
        }
    }

    for (int col = 0; col < VGA_WIDTH; col++) {
        VGA[vga_index(VGA_HEIGHT - 1, col)] =
            ((unsigned short)terminal_color << 8) | ' ';
    }

    cursor_row = VGA_HEIGHT - 1;
    cursor_col = 0;
    update_cursor();
}

void terminal_set_color(unsigned char color) {
    terminal_color = color;
}

void terminal_clear(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA[i] = ((unsigned short)terminal_color << 8) | ' ';
    }

    cursor_row = 0;
    cursor_col = 0;
    update_cursor();
}

void terminal_put_char(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        scroll_if_needed();
        update_cursor();
        return;
    }

    VGA[vga_index(cursor_row, cursor_col)] =
        ((unsigned short)terminal_color << 8) | c;

    cursor_col++;

    if (cursor_col >= VGA_WIDTH) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }

    update_cursor();
}

void terminal_backspace(void) {
    if (cursor_col > 0) {
        cursor_col--;
        VGA[vga_index(cursor_row, cursor_col)] =
            ((unsigned short)terminal_color << 8) | ' ';
    }
    update_cursor();
}

void terminal_print(const char* str) {
    int i = 0;
    while (str[i] != '\0') {
        terminal_put_char(str[i]);
        i++;
    }
}

void terminal_print_line(const char* str) {
    terminal_print(str);
    terminal_put_char('\n');
}

void terminal_init(void) {
    terminal_clear();
}