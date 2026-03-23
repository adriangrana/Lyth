#ifndef VIDEO_H
#define VIDEO_H

#include <stdint.h>
#include "multiboot.h"

typedef enum {
    VIDEO_BACKEND_NONE = 0,
    VIDEO_BACKEND_MULTIBOOT_FB = 1
} video_backend_t;

#define VIDEO_CAP_PRESENT_RGB32  (1U << 0)
#define VIDEO_CAP_FILL_RECT      (1U << 1)
#define VIDEO_CAP_DRAW_RECT      (1U << 2)
#define VIDEO_CAP_DRAW_LINE      (1U << 3)
#define VIDEO_CAP_MODESET        (1U << 4)

typedef struct {
    video_backend_t backend;
    uintptr_t framebuffer_addr;
    uint32_t pitch;
    uint32_t width;
    uint32_t height;
    uint8_t  bpp;
    uint8_t  type;
    /* RGB colour channel bit-positions and mask sizes within a pixel word.
     * Copied from the framebuffer driver which reads them from Multiboot. */
    uint8_t  red_pos;
    uint8_t  red_size;
    uint8_t  green_pos;
    uint8_t  green_size;
    uint8_t  blue_pos;
    uint8_t  blue_size;
    uint16_t vbe_mode;
    uint32_t caps;
} video_mode_t;

int video_init(multiboot_info_t* mbi);
int video_active(void);
void video_get_mode(video_mode_t* out_mode);
const char* video_backend_name(void);
const char* video_type_name(void);
uint32_t video_caps(void);
int video_set_mode(uint32_t width, uint32_t height, uint32_t bpp);

/* Drawing primitives — available when the corresponding VIDEO_CAP_* bit is set. */
void video_present_rgb32(const uint32_t* buffer, uint32_t width, uint32_t height,
                         uint32_t src_pitch_pixels);
void video_fill_rect(int x, int y, int width, int height, uint32_t rgb);
void video_draw_rect(int x, int y, int width, int height, uint32_t rgb);
void video_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb);

#endif