#include "terminal.h"
#include "fbconsole.h"
#include "timer.h"

#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile unsigned short* const VGA = (volatile unsigned short*)0xB8000;
static unsigned char terminal_color = 0x0F;

static int cursor_row = 0;
static int cursor_col = 0;

static int cursor_visible = 0;
static uint32_t last_blink = 0;

static inline void outb(unsigned short port, unsigned char value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static int terminal_width(void) {
    return fb_active() ? fb_columns() : VGA_WIDTH;
}

static int terminal_height(void) {
    return fb_active() ? fb_rows() : VGA_HEIGHT;
}

static int vga_index(int row, int col) {
    return row * VGA_WIDTH + col;
}

static void draw_cursor(void) {
    if (fb_active()) {
        fb_put_char_at(cursor_row, cursor_col, '_', terminal_color);
    } else {
        VGA[vga_index(cursor_row, cursor_col)] =
            ((unsigned short)terminal_color << 8) | '_';
    }
}

static void erase_cursor(void) {
    if (fb_active()) {
        fb_put_char_at(cursor_row, cursor_col, ' ', terminal_color);
    } else {
        VGA[vga_index(cursor_row, cursor_col)] =
            ((unsigned short)terminal_color << 8) | ' ';
    }
}

void terminal_update_cursor(void) {
    uint32_t now = timer_get_ticks();
    const uint32_t blink_interval = 50; /* PIT a 100 Hz -> 0.5 s */

    if ((now - last_blink) < blink_interval) {
        return;
    }

    last_blink = now;
    cursor_visible = !cursor_visible;

    if (cursor_visible) {
        draw_cursor();
    } else {
        erase_cursor();
    }
}

static void update_cursor(void) {
    if (fb_active()) {
        return;
    }

    unsigned short pos = (unsigned short)(cursor_row * VGA_WIDTH + cursor_col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));
    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void scroll_if_needed(void) {
    int width = terminal_width();
    int height = terminal_height();

    if (cursor_row < height) {
        return;
    }

    if (fb_active()) {
        fb_scroll(terminal_color);
        cursor_row = height - 1;
        cursor_col = 0;
        return;
    }

    for (int row = 1; row < height; row++) {
        for (int col = 0; col < width; col++) {
            VGA[vga_index(row - 1, col)] = VGA[vga_index(row, col)];
        }
    }

    for (int col = 0; col < width; col++) {
        VGA[vga_index(height - 1, col)] =
            ((unsigned short)terminal_color << 8) | ' ';
    }

    cursor_row = height - 1;
    cursor_col = 0;
    update_cursor();
}

void terminal_set_color(unsigned char color) {
    terminal_color = color;
}

void terminal_get_cursor(int* row, int* col) {
    if (row != 0) {
        *row = cursor_row;
    }

    if (col != 0) {
        *col = cursor_col;
    }
}

void terminal_set_cursor(int row, int col) {
    if (row < 0) {
        row = 0;
    }

    if (col < 0) {
        col = 0;
    }

    if (row >= terminal_height()) {
        row = terminal_height() - 1;
    }

    if (col >= terminal_width()) {
        col = terminal_width() - 1;
    }

    if (cursor_visible) {
        erase_cursor();
        cursor_visible = 0;
    }

    cursor_row = row;
    cursor_col = col;
    update_cursor();
}

void terminal_put_char_with_color(char c, unsigned char color) {
    unsigned char previous_color = terminal_color;

    terminal_color = color;
    terminal_put_char(c);
    terminal_color = previous_color;
}

void terminal_clear(void) {
    if (fb_active()) {
        fb_clear();
    } else {
        for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
            VGA[i] = ((unsigned short)terminal_color << 8) | ' ';
        }
    }

    cursor_row = 0;
    cursor_col = 0;
    cursor_visible = 0;
    last_blink = timer_get_ticks();
    update_cursor();
}

void terminal_put_char(char c) {
    if (cursor_visible) {
        erase_cursor();
        cursor_visible = 0;
    }

    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
        scroll_if_needed();
        update_cursor();
        return;
    }

    if (fb_active()) {
        fb_put_char_at(cursor_row, cursor_col, c, terminal_color);
    } else {
        VGA[vga_index(cursor_row, cursor_col)] =
            ((unsigned short)terminal_color << 8) | c;
    }

    cursor_col++;

    if (cursor_col >= terminal_width()) {
        cursor_col = 0;
        cursor_row++;
        scroll_if_needed();
    }

    update_cursor();
    last_blink = timer_get_ticks();
}

void terminal_backspace(void) {
    if (cursor_visible) {
        erase_cursor();
        cursor_visible = 0;
    }

    if (cursor_col > 0) {
        cursor_col--;
        if (fb_active()) {
            fb_put_char_at(cursor_row, cursor_col, ' ', terminal_color);
        } else {
            VGA[vga_index(cursor_row, cursor_col)] =
                ((unsigned short)terminal_color << 8) | ' ';
        }
    }

    update_cursor();
    last_blink = timer_get_ticks();
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

void terminal_print_uint(unsigned int value) {
    char digits[16];
    int length = 0;

    if (value == 0) {
        terminal_put_char('0');
        return;
    }

    while (value > 0 && length < (int)sizeof(digits)) {
        digits[length] = (char)('0' + (value % 10));
        value /= 10;
        length++;
    }

    while (length > 0) {
        length--;
        terminal_put_char(digits[length]);
    }
}

void terminal_init(void) {
    terminal_clear();
}