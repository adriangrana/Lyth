#include "video.h"
#include "fbconsole.h"

static video_mode_t current_mode;
static int video_is_active;

static void video_clear_mode(void) {
    current_mode.backend = VIDEO_BACKEND_NONE;
    current_mode.framebuffer_addr = 0;
    current_mode.pitch = 0;
    current_mode.width = 0;
    current_mode.height = 0;
    current_mode.bpp = 0;
    current_mode.type = 0;
    current_mode.vbe_mode = 0;
    current_mode.caps = 0;
}

int video_init(multiboot_info_t* mbi) {
    video_clear_mode();
    video_is_active = 0;

    if (!fb_init(mbi)) {
        return 0;
    }

    current_mode.backend = VIDEO_BACKEND_MULTIBOOT_FB;
    current_mode.framebuffer_addr = (uintptr_t)fb_get_buffer();
    current_mode.pitch = fb_pitch();
    current_mode.width = fb_width();
    current_mode.height = fb_height();
    current_mode.bpp = fb_bpp();
    current_mode.type = fb_type();
    current_mode.red_pos    = fb_red_pos();
    current_mode.red_size   = fb_red_size();
    current_mode.green_pos  = fb_green_pos();
    current_mode.green_size = fb_green_size();
    current_mode.blue_pos   = fb_blue_pos();
    current_mode.blue_size  = fb_blue_size();
    /* VBE mode number is only valid when bit 11 of Multiboot flags is set. */
    current_mode.vbe_mode = (mbi && (mbi->flags & (1u << 11))) ? mbi->vbe_mode : 0;
    current_mode.caps = VIDEO_CAP_PRESENT_RGB32 |
                        VIDEO_CAP_FILL_RECT |
                        VIDEO_CAP_DRAW_RECT |
                        VIDEO_CAP_DRAW_LINE;
    video_is_active = 1;
    return 1;
}

int video_active(void) {
    return video_is_active;
}

void video_get_mode(video_mode_t* out_mode) {
    if (!out_mode) {
        return;
    }

    *out_mode = current_mode;
}

const char* video_backend_name(void) {
    switch (current_mode.backend) {
    case VIDEO_BACKEND_MULTIBOOT_FB:
        return "multiboot-framebuffer";
    case VIDEO_BACKEND_NONE:
    default:
        return "none";
    }
}

const char* video_type_name(void) {
    return fb_type_name();
}

uint32_t video_caps(void) {
    return current_mode.caps;
}

int video_set_mode(uint32_t width, uint32_t height, uint32_t bpp) {
    (void)width;
    (void)height;
    (void)bpp;
    return -1;
}

void video_present_rgb32(const uint32_t* buffer, uint32_t width, uint32_t height,
                         uint32_t src_pitch_pixels) {
    if (video_is_active) {
        fb_present_rgb32(buffer, width, height, src_pitch_pixels);
    }
}

void video_fill_rect(int x, int y, int width, int height, uint32_t rgb) {
    if (video_is_active) {
        fb_fill_rect(x, y, width, height, rgb);
    }
}

void video_draw_rect(int x, int y, int width, int height, uint32_t rgb) {
    if (video_is_active) {
        fb_draw_rect(x, y, width, height, rgb);
    }
}

void video_draw_line(int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (video_is_active) {
        fb_draw_line(x0, y0, x1, y1, rgb);
    }
}