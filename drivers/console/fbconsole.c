#include "fbconsole.h"
#include "font_psf.h"
#include <stdint.h>

#define FONT_BITMAP_WIDTH   FONT_PSF_WIDTH
#define FONT_BITMAP_HEIGHT  FONT_PSF_HEIGHT

#define FONT_SCALE_X 1
#define FONT_SCALE_Y 1

#define FONT_WIDTH  (FONT_BITMAP_WIDTH * FONT_SCALE_X)
#define FONT_HEIGHT (FONT_BITMAP_HEIGHT * FONT_SCALE_Y)
#define FB_PADDING_X 8
#define FB_PADDING_Y 8

static framebuffer_info_t fbinfo;
static int fb_is_active = 0;

static const uint32_t vga_palette[16] = {
    0x000000, 0x0000AA, 0x00AA00, 0x00AAAA,
    0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
    0x555555, 0x5555FF, 0x55FF55, 0x55FFFF,
    0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF
};

static void fb_write_pixel(int x, int y, uint32_t rgb) {
    if (!fb_is_active || x < 0 || y < 0 ||
        (uint32_t)x >= fbinfo.width || (uint32_t)y >= fbinfo.height) {
        return;
    }

    uint8_t* pixel = (uint8_t*)fbinfo.addr + (y * fbinfo.pitch) + (x * (fbinfo.bpp / 8));

    if (fbinfo.bpp == 32) {
        pixel[0] = (uint8_t)(rgb & 0xFF);
        pixel[1] = (uint8_t)((rgb >> 8) & 0xFF);
        pixel[2] = (uint8_t)((rgb >> 16) & 0xFF);
        pixel[3] = 0;
    } else if (fbinfo.bpp == 24) {
        pixel[0] = (uint8_t)(rgb & 0xFF);
        pixel[1] = (uint8_t)((rgb >> 8) & 0xFF);
        pixel[2] = (uint8_t)((rgb >> 16) & 0xFF);
    }
}

static void fb_fill_rect(int x, int y, int width, int height, uint32_t rgb) {
    if (!fb_is_active || width <= 0 || height <= 0) {
        return;
    }

    for (int py = 0; py < height; py++) {
        for (int px = 0; px < width; px++) {
            fb_write_pixel(x + px, y + py, rgb);
        }
    }
}

static int fb_text_area_width(void) {
    if (!fb_is_active || fbinfo.width <= (uint32_t)(FB_PADDING_X * 2)) {
        return 0;
    }

    return (int)fbinfo.width - (FB_PADDING_X * 2);
}

static int fb_text_area_height(void) {
    if (!fb_is_active || fbinfo.height <= (uint32_t)(FB_PADDING_Y * 2)) {
        return 0;
    }

    return (int)fbinfo.height - (FB_PADDING_Y * 2);
}

static int fb_content_width(void) {
    return fb_columns() * FONT_WIDTH;
}

static int fb_content_height(void) {
    return fb_rows() * FONT_HEIGHT;
}

static void fb_copy_rect_up(int x, int y, int width, int height, int delta_y) {
    int bytes_per_pixel;
    uint8_t* base;

    if (!fb_is_active || width <= 0 || height <= 0 || delta_y <= 0) {
        return;
    }

    bytes_per_pixel = fbinfo.bpp / 8;
    base = (uint8_t*)fbinfo.addr;

    for (int row = 0; row < height; row++) {
        int src_y = y + row + delta_y;
        int dst_y = y + row;

        for (int col = 0; col < width; col++) {
            int src_x = x + col;
            int dst_x = x + col;
            uint8_t* src = base + (src_y * (int)fbinfo.pitch) + (src_x * bytes_per_pixel);
            uint8_t* dst = base + (dst_y * (int)fbinfo.pitch) + (dst_x * bytes_per_pixel);

            for (int byte = 0; byte < bytes_per_pixel; byte++) {
                dst[byte] = src[byte];
            }
        }
    }
}

int fb_init(multiboot_info_t* mbi) {
    if (!mbi) {
        fb_is_active = 0;
        return 0;
    }

    if ((mbi->flags & (1u << 12)) == 0 ||
        mbi->framebuffer_addr == 0 ||
        mbi->framebuffer_width == 0 ||
        mbi->framebuffer_height == 0) {
        fb_is_active = 0;
        return 0;
    }

    if (mbi->framebuffer_bpp != 24 && mbi->framebuffer_bpp != 32) {
        fb_is_active = 0;
        return 0;
    }

    fbinfo.addr   = (void*)(uintptr_t)mbi->framebuffer_addr;
    fbinfo.pitch  = mbi->framebuffer_pitch;
    fbinfo.width  = mbi->framebuffer_width;
    fbinfo.height = mbi->framebuffer_height;
    fbinfo.bpp    = mbi->framebuffer_bpp;
    fbinfo.type   = mbi->framebuffer_type;

    fb_is_active = 1;
    fb_clear();
    return 1;
}

int fb_active(void) {
    return fb_is_active;
}

int fb_rows(void) {
    return fb_is_active ? (fb_text_area_height() / FONT_HEIGHT) : 0;
}

int fb_columns(void) {
    return fb_is_active ? (fb_text_area_width() / FONT_WIDTH) : 0;
}

void fb_clear(void) {
    if (!fb_is_active) {
        return;
    }

    fb_fill_rect(0, 0, (int)fbinfo.width, (int)fbinfo.height, 0x000000);
}

void fb_scroll(unsigned char color) {
    int content_width;
    int content_height;

    if (!fb_is_active) {
        return;
    }

    content_width = fb_content_width();
    content_height = fb_content_height();

    if (content_width <= 0 || content_height <= FONT_HEIGHT) {
        fb_clear();
        return;
    }

    fb_copy_rect_up(
        FB_PADDING_X,
        FB_PADDING_Y,
        content_width,
        content_height - FONT_HEIGHT,
        FONT_HEIGHT
    );

    fb_fill_rect(
        FB_PADDING_X,
        FB_PADDING_Y + content_height - FONT_HEIGHT,
        content_width,
        FONT_HEIGHT,
        vga_palette[(color >> 4) & 0x0F]
    );
}

void fb_put_char_at(int row, int col, char c, unsigned char color) {
    if (!fb_is_active) {
        return;
    }

    if (row < 0 || row >= fb_rows() || col < 0 || col >= fb_columns()) {
        return;
    }

    unsigned int glyph = (unsigned char)c;

    if (glyph >= FONT_PSF_GLYPH_COUNT) {
        glyph = '?';
        if (glyph >= FONT_PSF_GLYPH_COUNT) {
            glyph = 0;
        }
    }

    uint32_t foreground = vga_palette[color & 0x0F];
    uint32_t background = vga_palette[(color >> 4) & 0x0F];
    
    int origin_x = col * FONT_WIDTH + FB_PADDING_X;
    int origin_y = row * FONT_HEIGHT + FB_PADDING_Y;

    fb_fill_rect(origin_x, origin_y, FONT_WIDTH, FONT_HEIGHT, background);

    for (int src_y = 0; src_y < FONT_BITMAP_HEIGHT; src_y++) {
        uint8_t bits = font_psf_data[glyph][src_y];

        for (int src_x = 0; src_x < FONT_BITMAP_WIDTH; src_x++) {
            /* En PSF/VGA el bit más significativo representa la columna izquierda. */
            if ((bits & (0x80u >> src_x)) != 0) {
                int dst_x = origin_x + (src_x * FONT_SCALE_X);
                int dst_y = origin_y + (src_y * FONT_SCALE_Y);

                for (int dy = 0; dy < FONT_SCALE_Y; dy++) {
                    for (int dx = 0; dx < FONT_SCALE_X; dx++) {
                        fb_write_pixel(dst_x + dx, dst_y + dy, foreground);
                    }
                }
            }
        }
    }
}

void fb_draw_cursor_at(int row, int col, unsigned char color) {
    int origin_x;
    int origin_y;
    int cursor_height;

    if (!fb_is_active) {
        return;
    }

    if (row < 0 || row >= fb_rows() || col < 0 || col >= fb_columns()) {
        return;
    }

    origin_x = col * FONT_WIDTH + FB_PADDING_X;
    origin_y = row * FONT_HEIGHT + FB_PADDING_Y;
    cursor_height = FONT_SCALE_Y < 2 ? 2 : FONT_SCALE_Y;

    fb_fill_rect(
        origin_x,
        origin_y + FONT_HEIGHT - cursor_height,
        FONT_WIDTH,
        cursor_height,
        vga_palette[color & 0x0F]
    );
}

uint32_t fb_width(void) {
    return fb_is_active ? fbinfo.width : 0;
}

uint32_t fb_height(void) {
    return fb_is_active ? fbinfo.height : 0;
}

uint32_t fb_pitch(void) {
    return fb_is_active ? fbinfo.pitch : 0;
}

uint8_t fb_bpp(void) {
    return fb_is_active ? fbinfo.bpp : 0;
}

uint8_t fb_type(void) {
    return fb_is_active ? fbinfo.type : 0;
}