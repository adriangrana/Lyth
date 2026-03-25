/* ============================================================
 *  viewer.c  —  Image / File Viewer
 *
 *  Displays BMP images (24-bit uncompressed) or falls back to
 *  text-mode rendering for non-image files.
 * ============================================================ */

#include "viewer.h"
#include "compositor.h"
#include "window.h"
#include "theme.h"
#include "font_psf.h"
#include "string.h"
#include "vfs.h"
#include "heap.h"

/* ---- Colours (from theme.h) ---- */
#define COL_V_BG      THEME_COL_BASE
#define COL_V_TEXT     THEME_COL_TEXT
#define COL_V_DIM      THEME_COL_DIM
#define COL_V_ACCENT   THEME_COL_ACCENT
#define COL_V_PANEL    THEME_COL_MANTLE
#define COL_V_BORDER   THEME_COL_BORDER

/* ---- Layout ---- */
#define VW_W           560
#define VW_H           420
#define VW_STATUS_H    20
#define VW_PAD         8
#define VW_MAX_BUF     (256 * 1024)  /* 256 KB file limit */
#define VW_TEXT_COLS   64
#define VW_TEXT_ROWS   22

/* ---- BMP Header (packed) ---- */
#pragma pack(push, 1)
typedef struct {
    uint16_t type;          /* 'BM' */
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t pixel_offset;
    uint32_t header_size;   /* DIB header size (40 for BITMAPINFOHEADER) */
    int32_t  width;
    int32_t  height;        /* positive = bottom-up, negative = top-down */
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
} bmp_header_t;
#pragma pack(pop)

/* ---- State ---- */
typedef struct {
    gui_window_t* win;
    char path[256];
    unsigned char* buf;
    int buf_size;

    /* image data (decoded pixels) */
    uint32_t* pixels;
    int img_w, img_h;
    int is_image;

    /* text mode state */
    int scroll;
    int line_count;
} viewer_state_t;

static viewer_state_t vst;
static int viewer_open;

/* ---- Helpers ---- */
static void int_to_str(unsigned int val, char* buf, int bufsz) {
    char tmp[12];
    int len = 0, i;
    if (val == 0) { tmp[len++] = '0'; }
    else { while (val) { tmp[len++] = '0' + (val % 10); val /= 10; } }
    for (i = 0; i < len && i < bufsz - 1; i++)
        buf[i] = tmp[len - 1 - i];
    buf[i] = '\0';
}

static int count_lines(const unsigned char* data, int size) {
    int n = 1, i;
    for (i = 0; i < size; i++)
        if (data[i] == '\n') n++;
    return n;
}

/* ---- BMP Decoder ---- */
static int decode_bmp(viewer_state_t* st) {
    bmp_header_t* hdr;
    int row, col;
    int abs_h;
    int bottom_up;
    unsigned char* pixel_data;
    int row_stride;

    if (st->buf_size < (int)sizeof(bmp_header_t)) return 0;

    hdr = (bmp_header_t*)st->buf;
    if (hdr->type != 0x4D42) return 0;  /* 'BM' little-endian */
    if (hdr->bpp != 24 && hdr->bpp != 32) return 0;
    if (hdr->compression != 0) return 0;
    if (hdr->width <= 0 || hdr->width > 2048) return 0;

    abs_h = hdr->height < 0 ? -hdr->height : hdr->height;
    if (abs_h <= 0 || abs_h > 2048) return 0;
    bottom_up = (hdr->height > 0);

    if ((int)hdr->pixel_offset >= st->buf_size) return 0;

    /* Allocate pixel buffer */
    st->pixels = (uint32_t*)kmalloc((unsigned int)(hdr->width * abs_h * 4));
    if (!st->pixels) return 0;

    st->img_w = hdr->width;
    st->img_h = abs_h;

    if (hdr->bpp == 24) {
        row_stride = ((hdr->width * 3 + 3) & ~3);  /* rows are 4-byte aligned */
    } else {
        row_stride = hdr->width * 4;
    }

    pixel_data = st->buf + hdr->pixel_offset;

    for (row = 0; row < abs_h; row++) {
        int src_row = bottom_up ? (abs_h - 1 - row) : row;
        unsigned char* src = pixel_data + src_row * row_stride;

        /* bounds check */
        if (src + (hdr->bpp == 24 ? hdr->width * 3 : hdr->width * 4) >
            st->buf + st->buf_size) break;

        for (col = 0; col < hdr->width; col++) {
            uint32_t px;
            if (hdr->bpp == 24) {
                uint8_t b = src[col * 3 + 0];
                uint8_t g = src[col * 3 + 1];
                uint8_t r = src[col * 3 + 2];
                px = (r << 16) | (g << 8) | b;
            } else {
                uint8_t b = src[col * 4 + 0];
                uint8_t g = src[col * 4 + 1];
                uint8_t r = src[col * 4 + 2];
                px = (r << 16) | (g << 8) | b;
            }
            st->pixels[row * hdr->width + col] = px;
        }
    }

    return 1;
}

/* ---- Load file ---- */
static void viewer_load(const char* path) {
    int fd, n;

    str_copy(vst.path, path, 256);

    /* Free previous */
    if (vst.buf) { kfree(vst.buf); vst.buf = 0; }
    if (vst.pixels) { kfree(vst.pixels); vst.pixels = 0; }
    vst.buf_size = 0;
    vst.is_image = 0;
    vst.img_w = vst.img_h = 0;
    vst.scroll = 0;
    vst.line_count = 0;

    fd = vfs_open(path);
    if (fd < 0) return;

    vst.buf = (unsigned char*)kmalloc(VW_MAX_BUF);
    if (!vst.buf) { vfs_close(fd); return; }

    n = vfs_read(fd, vst.buf, VW_MAX_BUF);
    vfs_close(fd);
    if (n <= 0) { kfree(vst.buf); vst.buf = 0; return; }
    vst.buf_size = n;

    /* Try BMP decode */
    if (decode_bmp(&vst)) {
        vst.is_image = 1;
    } else {
        /* Text mode */
        vst.line_count = count_lines(vst.buf, vst.buf_size);
    }
}

/* ---- Paint ---- */
static void viewer_paint(gui_window_t* win) {
    gui_surface_t* s = &win->surface;
    int cx = GUI_BORDER_WIDTH;
    int cy = GUI_TITLEBAR_HEIGHT;
    int cw = win->width - 2 * GUI_BORDER_WIDTH;
    int ch = win->height - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH - VW_STATUS_H;

    if (!s->pixels) return;
    gui_surface_clear(s, COL_V_BG);

    /* Decorations */
    gui_window_draw_decorations(win);

    if (!vst.buf) {
        gui_surface_draw_string(s, cx + VW_PAD, cy + VW_PAD,
                                "No file loaded. Open with File Manager.",
                                COL_V_DIM, 0, 0);
    } else if (vst.is_image && vst.pixels) {
        /* Blit image centered or at (0,0) if larger than viewport */
        int ox = cx + (cw > vst.img_w ? (cw - vst.img_w) / 2 : 0);
        int oy = cy + (ch > vst.img_h ? (ch - vst.img_h) / 2 : 0);
        int max_x = (vst.img_w < cw) ? vst.img_w : cw;
        int max_y = (vst.img_h < ch) ? vst.img_h : ch;
        int py, px;

        for (py = 0; py < max_y; py++) {
            for (px = 0; px < max_x; px++) {
                uint32_t col = vst.pixels[py * vst.img_w + px];
                gui_surface_putpixel(s, ox + px, oy + py, col);
            }
        }
    } else {
        /* Text mode — render lines */
        int vis_rows = ch / GUI_FONT_H;
        int line = 0, drawn = 0;
        int i = 0;
        int x0 = cx + VW_PAD;
        int y0 = cy + 4;

        /* Skip to scroll offset */
        while (i < vst.buf_size && line < vst.scroll) {
            if (vst.buf[i] == '\n') line++;
            i++;
        }

        while (i < vst.buf_size && drawn < vis_rows) {
            /* Find end of line */
            int start = i;
            while (i < vst.buf_size && vst.buf[i] != '\n') i++;

            int len = i - start;
            if (len > VW_TEXT_COLS) len = VW_TEXT_COLS;
            gui_surface_draw_string_n(s, x0, y0 + drawn * GUI_FONT_H,
                                      (const char*)&vst.buf[start], len,
                                      COL_V_TEXT, 0, 0);
            if (i < vst.buf_size) i++; /* skip newline */
            drawn++;
        }
    }

    /* Status bar */
    {
        int sy = win->height - GUI_BORDER_WIDTH - VW_STATUS_H;
        gui_surface_fill(s, cx, sy, cw, VW_STATUS_H, COL_V_PANEL);
        gui_surface_hline(s, cx, sy, cw, COL_V_BORDER);

        if (vst.buf) {
            char info[128];
            char num[12];
            str_copy(info, vst.path, 100);
            if (vst.is_image) {
                str_append(info, " | ", 128);
                int_to_str((unsigned int)vst.img_w, num, 12);
                str_append(info, num, 128);
                str_append(info, "x", 128);
                int_to_str((unsigned int)vst.img_h, num, 12);
                str_append(info, num, 128);
                str_append(info, " BMP", 128);
            } else {
                str_append(info, " | ", 128);
                int_to_str((unsigned int)vst.buf_size, num, 12);
                str_append(info, num, 128);
                str_append(info, " bytes", 128);
            }
            gui_surface_draw_string(s, cx + 4, sy + 3, info, COL_V_DIM, 0, 0);
        } else {
            gui_surface_draw_string(s, cx + 4, sy + 3, "No file", COL_V_DIM, 0, 0);
        }
    }
}

/* ---- Keyboard ---- */
static void viewer_key(gui_window_t* win, int event_type, char key) {
    if (event_type != 1) return; /* key press only */

    if (!vst.is_image && vst.buf) {
        int vis_rows = (win->height - GUI_TITLEBAR_HEIGHT - GUI_BORDER_WIDTH - VW_STATUS_H) / GUI_FONT_H;
        int max_scroll = vst.line_count - vis_rows;
        if (max_scroll < 0) max_scroll = 0;

        if (key == (char)0x48) { /* Up */
            if (vst.scroll > 0) vst.scroll--;
        } else if (key == (char)0x50) { /* Down */
            if (vst.scroll < max_scroll) vst.scroll++;
        } else if (key == (char)0x49) { /* PgUp */
            vst.scroll -= vis_rows;
            if (vst.scroll < 0) vst.scroll = 0;
        } else if (key == (char)0x51) { /* PgDn */
            vst.scroll += vis_rows;
            if (vst.scroll > max_scroll) vst.scroll = max_scroll;
        } else if (key == (char)0x47) { /* Home */
            vst.scroll = 0;
        } else if (key == (char)0x4F) { /* End */
            vst.scroll = max_scroll;
        }

        win->needs_redraw = 1;
        gui_dirty_add(win->x, win->y, win->width, win->height);
    }
}

/* ---- Close ---- */
static void viewer_close(gui_window_t* win) {
    if (vst.buf) { kfree(vst.buf); vst.buf = 0; }
    if (vst.pixels) { kfree(vst.pixels); vst.pixels = 0; }
    vst.buf_size = 0;
    vst.is_image = 0;
    viewer_open = 0;

    gui_dirty_add(win->x, win->y, win->width, win->height);
    gui_window_destroy(win);
    vst.win = 0;
}

/* ---- Public API ---- */
void viewer_app_open(void) {
    if (viewer_open && vst.win) {
        gui_window_focus(vst.win);
        return;
    }

    vst.win = gui_window_create("Viewer", 100, 60, VW_W, VW_H,
                                GUI_WIN_VISIBLE | GUI_WIN_CLOSEABLE |
                                GUI_WIN_DRAGGABLE | GUI_WIN_FOCUSED |
                                GUI_WIN_RESIZABLE);
    if (!vst.win) return;

    vst.win->on_paint = viewer_paint;
    vst.win->on_key   = viewer_key;
    vst.win->on_close = viewer_close;
    viewer_open = 1;

    vst.win->needs_redraw = 1;
    gui_dirty_add(vst.win->x, vst.win->y, vst.win->width, vst.win->height);
}

void viewer_app_open_file(const char* path) {
    viewer_app_open();
    if (!vst.win) return;

    viewer_load(path);

    /* Update window title */
    {
        /* Extract filename from path */
        const char* name = path;
        const char* p = path;
        while (*p) { if (*p == '/') name = p + 1; p++; }

        char title[GUI_MAX_TITLE];
        str_copy(title, "Viewer - ", GUI_MAX_TITLE);
        str_append(title, name, GUI_MAX_TITLE);
        str_copy(vst.win->title, title, GUI_MAX_TITLE);
    }

    vst.win->needs_redraw = 1;
    gui_dirty_add(vst.win->x, vst.win->y, vst.win->width, vst.win->height);
}
