/*
 * Lyth Software Renderer Backend
 *
 * CPU-only implementation of the gpu_ops_t interface.  Every draw call
 * operates directly on uint32_t pixel buffers (ARGB32, top-to-bottom,
 * left-to-right).  This is the reference backend — a future GPU backend
 * (e.g. virtio-gpu) would implement the same vtable using HW commands.
 */

#include "renderer.h"
#include "window.h"
#include "font_psf.h"
#include "fbconsole.h"
#include "physmem.h"
#include "string.h"
#include "timer.h"

/* Fast approximate divide by 255: exact for all inputs 0..65025 */
#define DIV255(x)  (((x) + 1 + ((x) >> 8)) >> 8)

/* ---- static state ---- */
static gpu_texture_t  sw_backbuffer;     /* main compositing surface       */
static gpu_texture_t *sw_target;         /* current render target          */
static gpu_clip_t     sw_clip;           /* active scissor                 */
static uint32_t       sw_next_id = 1;    /* texture id counter             */

/* framebuffer info cached at init */
static uint8_t *sw_fb;
static uint32_t sw_fb_pitch;
static int      sw_fb_bpp;

/* ---- max textures we can track (for wrap list) ---- */
#define SW_MAX_WRAPS 64
static gpu_texture_t sw_wraps[SW_MAX_WRAPS];
static int sw_wrap_count;

/* ================================================================== */
/*  Internal helpers                                                   */
/* ================================================================== */

/* Active target surface (never NULL) */
static inline gpu_texture_t *tgt(void) { return sw_target; }

/* Clip a rect (x,y,w,h) against the target bounds AND the active scissor.
 * Returns clipped rect in out_* ; returns 0 if fully clipped. */
static int clip_rect(int *x, int *y, int *w, int *h)
{
    gpu_texture_t *t = tgt();
    int x0 = *x, y0 = *y, x1 = *x + *w, y1 = *y + *h;

    /* target bounds */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > t->width)  x1 = t->width;
    if (y1 > t->height) y1 = t->height;

    /* scissor */
    if (sw_clip.active) {
        int cx0 = sw_clip.x, cy0 = sw_clip.y;
        int cx1 = sw_clip.x + sw_clip.w, cy1 = sw_clip.y + sw_clip.h;
        if (x0 < cx0) x0 = cx0;
        if (y0 < cy0) y0 = cy0;
        if (x1 > cx1) x1 = cx1;
        if (y1 > cy1) y1 = cy1;
    }

    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
    return (*w > 0 && *h > 0);
}

/* Fast 32-bit memset */
static void fill32(uint32_t *dst, uint32_t val, int count)
{
    int i;
    for (i = 0; i < count; i++) dst[i] = val;
}

/* ================================================================== */
/*  Lifecycle                                                          */
/* ================================================================== */

static int sw_init(int w, int h, int bpp)
{
    uint32_t sz = (uint32_t)(w * h) * 4;
    uint32_t phys = physmem_alloc_region(sz, 4096);
    if (!phys) return -1;

    sw_backbuffer.id         = sw_next_id++;
    sw_backbuffer.width      = w;
    sw_backbuffer.height     = h;
    sw_backbuffer.stride     = w;
    sw_backbuffer.pixels     = (uint32_t *)(uintptr_t)phys;
    sw_backbuffer.alloc_phys = phys;
    sw_backbuffer.alloc_size = sz;
    sw_backbuffer.flags      = GPU_TEX_TARGET;
    sw_backbuffer._gpu_handle = 0;
    memset(sw_backbuffer.pixels, 0, sz);

    sw_target = &sw_backbuffer;
    sw_clip.active = 0;
    sw_wrap_count = 0;

    /* cache framebuffer info */
    sw_fb       = (uint8_t *)fb_get_buffer();
    sw_fb_pitch = fb_pitch();
    sw_fb_bpp   = (int)fb_bpp();

    return 0;
}

static void sw_shutdown(void)
{
    if (sw_backbuffer.alloc_phys) {
        physmem_free_region(sw_backbuffer.alloc_phys, sw_backbuffer.alloc_size);
        sw_backbuffer.pixels = 0;
        sw_backbuffer.alloc_phys = 0;
    }
}

/* ================================================================== */
/*  Texture management                                                 */
/* ================================================================== */

static gpu_texture_t *sw_texture_create(int w, int h, uint32_t flags)
{
    uint32_t sz, phys;
    gpu_texture_t *t;
    if (w <= 0 || h <= 0) return 0;
    if (sw_wrap_count >= SW_MAX_WRAPS) return 0;

    sz = (uint32_t)(w * h) * 4;
    phys = physmem_alloc_region(sz, 4096);
    if (!phys) return 0;

    t = &sw_wraps[sw_wrap_count++];
    t->id          = sw_next_id++;
    t->width       = w;
    t->height      = h;
    t->stride      = w;
    t->pixels      = (uint32_t *)(uintptr_t)phys;
    t->alloc_phys  = phys;
    t->alloc_size  = sz;
    t->flags       = flags;
    t->_gpu_handle = 0;
    memset(t->pixels, 0, sz);
    return t;
}

static void sw_texture_destroy(gpu_texture_t *t)
{
    if (!t) return;
    if (t->alloc_phys && !(t->flags & GPU_TEX_WRAPPED)) {
        physmem_free_region(t->alloc_phys, t->alloc_size);
    }
    t->pixels     = 0;
    t->alloc_phys = 0;
    t->alloc_size = 0;
    t->id         = 0;
}

static void sw_texture_upload(gpu_texture_t *t, int x, int y, int w, int h,
                              const uint32_t *pixels)
{
    int row;
    if (!t || !t->pixels || !pixels) return;
    if (x < 0) { w += x; pixels -= x; x = 0; }
    if (y < 0) { h += y; pixels -= y * w; y = 0; }
    if (x + w > t->width)  w = t->width - x;
    if (y + h > t->height) h = t->height - y;
    if (w <= 0 || h <= 0) return;
    for (row = 0; row < h; row++) {
        memcpy(&t->pixels[(y + row) * t->stride + x],
               &pixels[row * w], (size_t)w * 4);
    }
}

static void sw_texture_download(gpu_texture_t *t, int x, int y, int w, int h,
                                uint32_t *out)
{
    int row;
    if (!t || !t->pixels || !out) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > t->width)  w = t->width - x;
    if (y + h > t->height) h = t->height - y;
    if (w <= 0 || h <= 0) return;
    for (row = 0; row < h; row++) {
        memcpy(&out[row * w],
               &t->pixels[(y + row) * t->stride + x],
               (size_t)w * 4);
    }
}

/* ================================================================== */
/*  Render target                                                      */
/* ================================================================== */

static void sw_set_target(gpu_texture_t *t)
{
    sw_target = t ? t : &sw_backbuffer;
}

static gpu_texture_t *sw_get_target(void)
{
    return sw_target;
}

/* ================================================================== */
/*  Clip / scissor                                                     */
/* ================================================================== */

static void sw_set_clip(int x, int y, int w, int h)
{
    sw_clip.x = x;
    sw_clip.y = y;
    sw_clip.w = w;
    sw_clip.h = h;
    sw_clip.active = 1;
}

static void sw_clear_clip(void)
{
    sw_clip.active = 0;
}

/* ================================================================== */
/*  Clear                                                              */
/* ================================================================== */

static void sw_clear(uint32_t color)
{
    gpu_texture_t *t = tgt();
    int x = 0, y = 0, w = t->width, h = t->height;
    int row;
    if (!clip_rect(&x, &y, &w, &h)) return;
    for (row = y; row < y + h; row++)
        fill32(&t->pixels[row * t->stride + x], color, w);
}

/* ================================================================== */
/*  Drawing primitives                                                 */
/* ================================================================== */

static void sw_fill_rect(int x, int y, int w, int h, uint32_t color)
{
    gpu_texture_t *t = tgt();
    int row;
    if (!clip_rect(&x, &y, &w, &h)) return;
    for (row = y; row < y + h; row++)
        fill32(&t->pixels[row * t->stride + x], color, w);
}

static void sw_fill_rect_alpha(int x, int y, int w, int h,
                                uint32_t col, int alpha)
{
    gpu_texture_t *t = tgt();
    int row, cx;
    uint32_t fr, fg, fb_c;
    int ia;
    if (alpha <= 0) return;
    if (alpha >= 255) { sw_fill_rect(x, y, w, h, col); return; }
    if (!clip_rect(&x, &y, &w, &h)) return;
    fr = (col >> 16) & 0xFF;
    fg = (col >> 8)  & 0xFF;
    fb_c = col & 0xFF;
    ia = 255 - alpha;
    for (row = y; row < y + h; row++) {
        uint32_t *p = &t->pixels[row * t->stride + x];
        for (cx = 0; cx < w; cx++) {
            uint32_t bg = p[cx];
            uint32_t r = DIV255(fr * (uint32_t)alpha + ((bg >> 16) & 0xFF) * (uint32_t)ia);
            uint32_t g = DIV255(fg * (uint32_t)alpha + ((bg >> 8)  & 0xFF) * (uint32_t)ia);
            uint32_t b = DIV255(fb_c * (uint32_t)alpha + (bg & 0xFF) * (uint32_t)ia);
            p[cx] = (r << 16) | (g << 8) | b;
        }
    }
}

static void sw_fill_rounded(int x, int y, int w, int h, int r, uint32_t col)
{
    if (r < 1 || h < 4 || w < 4) { sw_fill_rect(x, y, w, h, col); return; }
    if (r > 3) r = 3;
    if (r == 1) {
        sw_fill_rect(x + 1, y, w - 2, 1, col);
        sw_fill_rect(x, y + 1, w, h - 2, col);
        sw_fill_rect(x + 1, y + h - 1, w - 2, 1, col);
    } else if (r == 2) {
        sw_fill_rect(x + 2, y, w - 4, 1, col);
        sw_fill_rect(x + 1, y + 1, w - 2, 1, col);
        sw_fill_rect(x, y + 2, w, h - 4, col);
        sw_fill_rect(x + 1, y + h - 2, w - 2, 1, col);
        sw_fill_rect(x + 2, y + h - 1, w - 4, 1, col);
    } else {
        sw_fill_rect(x + 3, y, w - 6, 1, col);
        sw_fill_rect(x + 2, y + 1, w - 4, 1, col);
        sw_fill_rect(x + 1, y + 2, w - 2, 1, col);
        sw_fill_rect(x, y + 3, w, h - 6, col);
        sw_fill_rect(x + 1, y + h - 3, w - 2, 1, col);
        sw_fill_rect(x + 2, y + h - 2, w - 4, 1, col);
        sw_fill_rect(x + 3, y + h - 1, w - 6, 1, col);
    }
}

static void sw_fill_rounded_alpha(int x, int y, int w, int h, int r,
                                   uint32_t col, int alpha)
{
    if (alpha >= 255) { sw_fill_rounded(x, y, w, h, r, col); return; }
    if (alpha <= 0) return;
    if (r < 1 || h < 4 || w < 4) {
        sw_fill_rect_alpha(x, y, w, h, col, alpha); return;
    }
    if (r > 3) r = 3;
    if (r == 1) {
        sw_fill_rect_alpha(x + 1, y, w - 2, 1, col, alpha);
        sw_fill_rect_alpha(x, y + 1, w, h - 2, col, alpha);
        sw_fill_rect_alpha(x + 1, y + h - 1, w - 2, 1, col, alpha);
    } else if (r == 2) {
        sw_fill_rect_alpha(x + 2, y, w - 4, 1, col, alpha);
        sw_fill_rect_alpha(x + 1, y + 1, w - 2, 1, col, alpha);
        sw_fill_rect_alpha(x, y + 2, w, h - 4, col, alpha);
        sw_fill_rect_alpha(x + 1, y + h - 2, w - 2, 1, col, alpha);
        sw_fill_rect_alpha(x + 2, y + h - 1, w - 4, 1, col, alpha);
    } else {
        sw_fill_rect_alpha(x + 3, y, w - 6, 1, col, alpha);
        sw_fill_rect_alpha(x + 2, y + 1, w - 4, 1, col, alpha);
        sw_fill_rect_alpha(x + 1, y + 2, w - 2, 1, col, alpha);
        sw_fill_rect_alpha(x, y + 3, w, h - 6, col, alpha);
        sw_fill_rect_alpha(x + 1, y + h - 3, w - 2, 1, col, alpha);
        sw_fill_rect_alpha(x + 2, y + h - 2, w - 4, 1, col, alpha);
        sw_fill_rect_alpha(x + 3, y + h - 1, w - 6, 1, col, alpha);
    }
}

static void sw_hline(int x, int y, int w, uint32_t color)
{
    int h = 1;
    if (!clip_rect(&x, &y, &w, &h)) return;
    fill32(&tgt()->pixels[y * tgt()->stride + x], color, w);
}

static void sw_putpixel(int x, int y, uint32_t color)
{
    gpu_texture_t *t = tgt();
    if (x < 0 || y < 0 || x >= t->width || y >= t->height) return;
    if (sw_clip.active) {
        if (x < sw_clip.x || y < sw_clip.y ||
            x >= sw_clip.x + sw_clip.w || y >= sw_clip.y + sw_clip.h)
            return;
    }
    t->pixels[y * t->stride + x] = color;
}

/* ================================================================== */
/*  Blit (opaque copy from texture)                                    */
/* ================================================================== */

static void sw_blit(int dx, int dy, gpu_texture_t *src,
                    int sx, int sy, int sw_w, int sh)
{
    gpu_texture_t *t = tgt();
    int row;
    if (!src || !src->pixels || !t->pixels) return;

    /* clip source */
    if (sx < 0) { dx -= sx; sw_w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw_w > src->width)  sw_w = src->width - sx;
    if (sy + sh > src->height)   sh = src->height - sy;

    /* clip dest */
    if (dx < 0) { sx -= dx; sw_w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw_w > t->width)  sw_w = t->width - dx;
    if (dy + sh > t->height)   sh = t->height - dy;

    /* scissor */
    if (sw_clip.active) {
        int cx0 = sw_clip.x, cy0 = sw_clip.y;
        int cx1 = sw_clip.x + sw_clip.w, cy1 = sw_clip.y + sw_clip.h;
        if (dx < cx0) { int d = cx0 - dx; sx += d; sw_w -= d; dx = cx0; }
        if (dy < cy0) { int d = cy0 - dy; sy += d; sh -= d; dy = cy0; }
        if (dx + sw_w > cx1) sw_w = cx1 - dx;
        if (dy + sh > cy1) sh = cy1 - dy;
    }

    if (sw_w <= 0 || sh <= 0) return;
    for (row = 0; row < sh; row++) {
        memcpy(&t->pixels[(dy + row) * t->stride + dx],
               &src->pixels[(sy + row) * src->stride + sx],
               (size_t)sw_w * 4);
    }
}

static void sw_blit_scaled(int dx, int dy, int dw, int dh,
                           gpu_texture_t *src, int sx, int sy, int sw_w, int sh)
{
    gpu_texture_t *t = tgt();
    int row, col;
    if (!src || !src->pixels || !t->pixels) return;
    if (dw <= 0 || dh <= 0 || sw_w <= 0 || sh <= 0) return;

    for (row = 0; row < dh; row++) {
        int py = dy + row;
        if (py < 0 || py >= t->height) continue;
        if (sw_clip.active && (py < sw_clip.y || py >= sw_clip.y + sw_clip.h))
            continue;
        int sr = sy + row * sh / dh;
        if (sr >= src->height) continue;
        for (col = 0; col < dw; col++) {
            int px = dx + col;
            if (px < 0 || px >= t->width) continue;
            if (sw_clip.active && (px < sw_clip.x || px >= sw_clip.x + sw_clip.w))
                continue;
            int sc = sx + col * sw_w / dw;
            if (sc >= src->width) continue;
            t->pixels[py * t->stride + px] = src->pixels[sr * src->stride + sc];
        }
    }
}

/* ================================================================== */
/*  Blit with uniform alpha (window opacity)                           */
/* ================================================================== */

static void sw_blit_alpha(int dx, int dy, gpu_texture_t *src,
                          int sx, int sy, int sw_w, int sh, int alpha)
{
    gpu_texture_t *t = tgt();
    int row, col;
    if (!src || !src->pixels || !t->pixels) return;
    if (alpha <= 0) return;
    if (alpha >= 255) { sw_blit(dx, dy, src, sx, sy, sw_w, sh); return; }

    /* clip source */
    if (sx < 0) { dx -= sx; sw_w += sx; sx = 0; }
    if (sy < 0) { dy -= sy; sh += sy; sy = 0; }
    if (sx + sw_w > src->width)  sw_w = src->width - sx;
    if (sy + sh > src->height)   sh = src->height - sy;

    /* clip dest */
    if (dx < 0) { sx -= dx; sw_w += dx; dx = 0; }
    if (dy < 0) { sy -= dy; sh += dy; dy = 0; }
    if (dx + sw_w > t->width)  sw_w = t->width - dx;
    if (dy + sh > t->height)   sh = t->height - dy;

    /* scissor */
    if (sw_clip.active) {
        int cx0 = sw_clip.x, cy0 = sw_clip.y;
        int cx1 = sw_clip.x + sw_clip.w, cy1 = sw_clip.y + sw_clip.h;
        if (dx < cx0) { int d = cx0 - dx; sx += d; sw_w -= d; dx = cx0; }
        if (dy < cy0) { int d = cy0 - dy; sy += d; sh -= d; dy = cy0; }
        if (dx + sw_w > cx1) sw_w = cx1 - dx;
        if (dy + sh > cy1) sh = cy1 - dy;
    }

    if (sw_w <= 0 || sh <= 0) return;
    int ia = 255 - alpha;
    for (row = 0; row < sh; row++) {
        uint32_t *dp = &t->pixels[(dy + row) * t->stride + dx];
        uint32_t *sp = &src->pixels[(sy + row) * src->stride + sx];
        for (col = 0; col < sw_w; col++) {
            uint32_t s = sp[col];
            uint32_t d = dp[col];
            int r = (int)DIV255((uint32_t)((s >> 16) & 0xFF) * (uint32_t)alpha + (uint32_t)((d >> 16) & 0xFF) * (uint32_t)ia);
            int g = (int)DIV255((uint32_t)((s >> 8)  & 0xFF) * (uint32_t)alpha + (uint32_t)((d >> 8)  & 0xFF) * (uint32_t)ia);
            int b = (int)DIV255((uint32_t)(s & 0xFF)         * (uint32_t)alpha + (uint32_t)(d & 0xFF)         * (uint32_t)ia);
            dp[col] = ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        }
    }
}

/* ================================================================== */
/*  Icon blit (per-pixel alpha from ARGB data)                         */
/* ================================================================== */

static void sw_blit_icon(int dx, int dy,
                         const uint32_t *pixels, int iw, int ih)
{
    gpu_texture_t *t = tgt();
    int row, col;
    if (!pixels || !t->pixels) return;
    for (row = 0; row < ih; row++) {
        int py = dy + row;
        if (py < 0 || py >= t->height) continue;
        if (sw_clip.active && (py < sw_clip.y || py >= sw_clip.y + sw_clip.h))
            continue;
        for (col = 0; col < iw; col++) {
            int px = dx + col;
            if (px < 0 || px >= t->width) continue;
            if (sw_clip.active && (px < sw_clip.x || px >= sw_clip.x + sw_clip.w))
                continue;
            uint32_t p = pixels[row * iw + col];
            int a = (int)((p >> 24) & 0xFF);
            if (a == 0) continue;
            if (a == 255) {
                t->pixels[py * t->stride + px] = p & 0x00FFFFFF;
            } else {
                uint32_t bg = t->pixels[py * t->stride + px];
                int ia = 255 - a;
                int r = (int)DIV255((uint32_t)((p >> 16) & 0xFF) * (uint32_t)a + (uint32_t)((bg >> 16) & 0xFF) * (uint32_t)ia);
                int g = (int)DIV255((uint32_t)((p >> 8) & 0xFF) * (uint32_t)a + (uint32_t)((bg >> 8) & 0xFF) * (uint32_t)ia);
                int b = (int)DIV255((uint32_t)(p & 0xFF) * (uint32_t)a + (uint32_t)(bg & 0xFF) * (uint32_t)ia);
                t->pixels[py * t->stride + px] =
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}

static void sw_blit_icon_scaled(int dx, int dy, int dw, int dh,
                                const uint32_t *pixels, int iw, int ih)
{
    gpu_texture_t *t = tgt();
    int row, col;
    if (!pixels || !t->pixels || dw <= 0 || dh <= 0) return;
    for (row = 0; row < dh; row++) {
        int py = dy + row;
        if (py < 0 || py >= t->height) continue;
        if (sw_clip.active && (py < sw_clip.y || py >= sw_clip.y + sw_clip.h))
            continue;
        int sr = row * ih / dh;
        for (col = 0; col < dw; col++) {
            int px = dx + col;
            if (px < 0 || px >= t->width) continue;
            if (sw_clip.active && (px < sw_clip.x || px >= sw_clip.x + sw_clip.w))
                continue;
            int sc = col * iw / dw;
            uint32_t p = pixels[sr * iw + sc];
            int a = (int)((p >> 24) & 0xFF);
            if (a == 0) continue;
            if (a == 255) {
                t->pixels[py * t->stride + px] = p & 0x00FFFFFF;
            } else {
                uint32_t bg = t->pixels[py * t->stride + px];
                int ia = 255 - a;
                int r = (int)DIV255((uint32_t)((p >> 16) & 0xFF) * (uint32_t)a + (uint32_t)((bg >> 16) & 0xFF) * (uint32_t)ia);
                int g = (int)DIV255((uint32_t)((p >> 8) & 0xFF) * (uint32_t)a + (uint32_t)((bg >> 8) & 0xFF) * (uint32_t)ia);
                int b = (int)DIV255((uint32_t)(p & 0xFF) * (uint32_t)a + (uint32_t)(bg & 0xFF) * (uint32_t)ia);
                t->pixels[py * t->stride + px] =
                    ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
            }
        }
    }
}

/* ================================================================== */
/*  Text                                                               */
/* ================================================================== */

static void sw_draw_char(int x, int y, unsigned char ch,
                         uint32_t fg, uint32_t bg, int use_bg)
{
    gpu_texture_t *t = tgt();
    int row, col;
    uint8_t bits;
    if (!t->pixels) return;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    if (x + FONT_PSF_WIDTH <= 0 || x >= t->width ||
        y + FONT_PSF_HEIGHT <= 0 || y >= t->height) return;
    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= t->height) continue;
        if (sw_clip.active && (py < sw_clip.y || py >= sw_clip.y + sw_clip.h))
            continue;
        bits = font_psf_data[ch][row];
        uint32_t *dst = &t->pixels[py * t->stride];
        for (col = 0; col < FONT_PSF_WIDTH; col++) {
            int px = x + col;
            if (px < 0 || px >= t->width) continue;
            if (sw_clip.active && (px < sw_clip.x || px >= sw_clip.x + sw_clip.w))
                continue;
            if (bits & (0x80u >> col))
                dst[px] = fg;
            else if (use_bg)
                dst[px] = bg;
        }
    }
}

static void sw_draw_string(int x, int y, const char *s,
                           uint32_t fg, uint32_t bg, int use_bg)
{
    if (!s) return;
    while (*s) {
        sw_draw_char(x, y, (unsigned char)*s, fg, bg, use_bg);
        x += FONT_PSF_WIDTH;
        s++;
    }
}

static void sw_draw_char_alpha(int x, int y, unsigned char ch,
                                uint32_t col, int alpha)
{
    gpu_texture_t *t = tgt();
    int row, c;
    uint8_t bits;
    uint32_t fr, fg, fb_c;
    int ia;
    if (!t->pixels || alpha <= 0) return;
    if (ch >= FONT_PSF_GLYPH_COUNT) ch = '?';
    if (x + FONT_PSF_WIDTH <= 0 || x >= t->width ||
        y + FONT_PSF_HEIGHT <= 0 || y >= t->height) return;
    fr = (col >> 16) & 0xFF;
    fg = (col >> 8) & 0xFF;
    fb_c = col & 0xFF;
    ia = 255 - alpha;
    for (row = 0; row < FONT_PSF_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= t->height) continue;
        if (sw_clip.active && (py < sw_clip.y || py >= sw_clip.y + sw_clip.h))
            continue;
        bits = font_psf_data[ch][row];
        uint32_t *dst = &t->pixels[py * t->stride];
        for (c = 0; c < FONT_PSF_WIDTH; c++) {
            int px = x + c;
            if (px < 0 || px >= t->width) continue;
            if (sw_clip.active && (px < sw_clip.x || px >= sw_clip.x + sw_clip.w))
                continue;
            if (bits & (0x80u >> c)) {
                if (alpha >= 255) {
                    dst[px] = col;
                } else {
                    uint32_t bg = dst[px];
                    uint32_t r = DIV255(fr * (uint32_t)alpha + ((bg >> 16) & 0xFF) * (uint32_t)ia);
                    uint32_t g = DIV255(fg * (uint32_t)alpha + ((bg >> 8) & 0xFF) * (uint32_t)ia);
                    uint32_t b = DIV255(fb_c * (uint32_t)alpha + (bg & 0xFF) * (uint32_t)ia);
                    dst[px] = (r << 16) | (g << 8) | b;
                }
            }
        }
    }
}

/* ================================================================== */
/*  Effects                                                            */
/* ================================================================== */

static void sw_box_blur(int x, int y, int w, int h, int passes)
{
    gpu_texture_t *t = tgt();
    int row, col, ky, kx, pass;
    int sx = t->stride;
    int dw = t->width, dh = t->height;

    if (!clip_rect(&x, &y, &w, &h)) return;

    for (pass = 0; pass < passes; pass++) {
        for (row = y; row < y + h; row++) {
            for (col = x; col < x + w; col++) {
                unsigned int sr = 0, sg = 0, sb = 0, cnt = 0;
                for (ky = -2; ky <= 2; ky++) {
                    int py = row + ky;
                    if (py < 0 || py >= dh) continue;
                    for (kx = -2; kx <= 2; kx++) {
                        int px = col + kx;
                        if (px < 0 || px >= dw) continue;
                        uint32_t p = t->pixels[py * sx + px];
                        sr += (p >> 16) & 0xFF;
                        sg += (p >> 8)  & 0xFF;
                        sb += p & 0xFF;
                        cnt++;
                    }
                }
                if (cnt > 0)
                    t->pixels[row * sx + col] = ((sr / cnt) << 16) |
                                                 ((sg / cnt) << 8) |
                                                 (sb / cnt);
            }
        }
    }
}

static void sw_shadow(int wx, int wy, int ww, int wh,
                      int off, int spread, int alpha)
{
    gpu_texture_t *t = tgt();
    /* pre-compute fixed-point inverse: (255 - alpha) * 257 ≈ /255 via >>16
     * We use: (c * ia + 128) >> 8  as a fast approximation of c*(255-alpha)/255
     * but even faster: multiply by ia_fix >> 16 where ia_fix = ia * 257 */
    uint32_t ia = (uint32_t)(255 - alpha);
    uint32_t ia_fix = (ia << 8) + ia;  /* ia * 257 — fits in 16 bits */
    int shx = wx + off, shy = wy + off;
    int shw = ww + spread, shh = wh + spread;
    int row, cx;

    /* clip to target + scissor */
    int s0x = shx, s0y = shy;
    int s1x = shx + shw, s1y = shy + shh;
    if (s0x < 0) s0x = 0;
    if (s0y < 0) s0y = 0;
    if (s1x > t->width) s1x = t->width;
    if (s1y > t->height) s1y = t->height;
    if (sw_clip.active) {
        if (s0x < sw_clip.x) s0x = sw_clip.x;
        if (s0y < sw_clip.y) s0y = sw_clip.y;
        if (s1x > sw_clip.x + sw_clip.w) s1x = sw_clip.x + sw_clip.w;
        if (s1y > sw_clip.y + sw_clip.h) s1y = sw_clip.y + sw_clip.h;
    }
    if (s0x >= s1x || s0y >= s1y) return;

    for (row = s0y; row < s1y; row++) {
        uint32_t *p = &t->pixels[row * t->stride + s0x];
        /* fast skip: if entire row is inside window, nothing to draw */
        if (row >= wy && row < wy + wh && s0x >= wx && s1x <= wx + ww)
            continue;
        for (cx = 0; cx < s1x - s0x; cx++) {
            int px = s0x + cx, py = row;
            /* skip pixels inside the window itself */
            if (px >= wx && px < wx + ww && py >= wy && py < wy + wh)
                continue;
            uint32_t bg = p[cx];
            /* fast darkening: multiply each channel by ia/255 using
             * fixed-point: (ch * ia_fix) >> 16 ≈ ch * ia / 255 */
            uint32_t r = (((bg >> 16) & 0xFF) * ia_fix) >> 16;
            uint32_t g = (((bg >> 8)  & 0xFF) * ia_fix) >> 16;
            uint32_t b = ((bg & 0xFF)          * ia_fix) >> 16;
            p[cx] = (r << 16) | (g << 8) | b;
        }
    }
}

/* ================================================================== */
/*  Batched quad submission (SW: just iterate and draw)                 */
/* ================================================================== */

static void sw_submit_quads(const gpu_quad_t *quads, int count)
{
    int i;
    for (i = 0; i < count; i++) {
        const gpu_quad_t *q = &quads[i];
        if (q->texture) {
            if (q->dw == q->sw && q->dh == q->sh) {
                sw_blit(q->dx, q->dy, q->texture,
                        q->sx, q->sy, q->sw, q->sh);
            } else {
                sw_blit_scaled(q->dx, q->dy, q->dw, q->dh,
                               q->texture, q->sx, q->sy, q->sw, q->sh);
            }
        } else {
            if (q->alpha < 255)
                sw_fill_rect_alpha(q->dx, q->dy, q->dw, q->dh,
                                   q->color, (int)q->alpha);
            else
                sw_fill_rect(q->dx, q->dy, q->dw, q->dh, q->color);
        }
    }
}

/* ================================================================== */
/*  Present (copy backbuffer → framebuffer)                            */
/* ================================================================== */

static void sw_present(int x, int y, int w, int h)
{
    int row;
    if (!sw_fb) {
        sw_fb = (uint8_t *)fb_get_buffer();
        if (!sw_fb) return;
    }

    /* clip to screen */
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > sw_backbuffer.width)  w = sw_backbuffer.width - x;
    if (y + h > sw_backbuffer.height) h = sw_backbuffer.height - y;
    if (w <= 0 || h <= 0) return;

    if (sw_fb_bpp == 32) {
        for (row = y; row < y + h; row++) {
            memcpy(sw_fb + row * sw_fb_pitch + x * 4,
                   &sw_backbuffer.pixels[row * sw_backbuffer.stride + x],
                   (size_t)w * 4);
        }
    } else {
        fb_present_rgb32(sw_backbuffer.pixels,
                         (uint32_t)sw_backbuffer.width,
                         (uint32_t)sw_backbuffer.height,
                         (uint32_t)sw_backbuffer.stride);
    }
    /* Flush WC buffers so the display controller sees complete data */
    __asm__ volatile("sfence" ::: "memory");
}

static void sw_present_full(void)
{
    sw_present(0, 0, sw_backbuffer.width, sw_backbuffer.height);
}

/* inline port I/O for vsync */
static inline uint8_t sw_inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static void sw_vsync_wait(void)
{
    unsigned int t0 = timer_get_monotonic_us();
    /* Phase 1: if already in retrace, wait for it to end */
    while (sw_inb(0x3DA) & 0x08) {
        if (timer_get_monotonic_us() - t0 > 18000) return;
    }
    /* Phase 2: wait for retrace to start */
    while (!(sw_inb(0x3DA) & 0x08)) {
        if (timer_get_monotonic_us() - t0 > 18000) return;
    }
}

/* ================================================================== */
/*  VTable + global renderer                                           */
/* ================================================================== */

const gpu_ops_t sw_ops = {
    .name               = "software",
    .init               = sw_init,
    .shutdown           = sw_shutdown,
    .texture_create     = sw_texture_create,
    .texture_destroy    = sw_texture_destroy,
    .texture_upload     = sw_texture_upload,
    .texture_download   = sw_texture_download,
    .set_target         = sw_set_target,
    .get_target         = sw_get_target,
    .set_clip           = sw_set_clip,
    .clear_clip         = sw_clear_clip,
    .clear              = sw_clear,
    .fill_rect          = sw_fill_rect,
    .fill_rect_alpha    = sw_fill_rect_alpha,
    .fill_rounded       = sw_fill_rounded,
    .fill_rounded_alpha = sw_fill_rounded_alpha,
    .hline              = sw_hline,
    .putpixel           = sw_putpixel,
    .blit               = sw_blit,
    .blit_scaled        = sw_blit_scaled,
    .blit_alpha         = sw_blit_alpha,
    .blit_icon          = sw_blit_icon,
    .blit_icon_scaled   = sw_blit_icon_scaled,
    .draw_char          = sw_draw_char,
    .draw_string        = sw_draw_string,
    .draw_char_alpha    = sw_draw_char_alpha,
    .box_blur           = sw_box_blur,
    .shadow             = sw_shadow,
    .submit_quads       = sw_submit_quads,
    .present            = sw_present,
    .present_full       = sw_present_full,
    .vsync_wait         = sw_vsync_wait,
};

/* The single global renderer instance */
gpu_renderer_t g_gpu;

int gpu_init_sw(int w, int h, int bpp)
{
    int rc;
    g_gpu.ops = &sw_ops;
    g_gpu.width = w;
    g_gpu.height = h;
    g_gpu.bpp = bpp;
    rc = sw_init(w, h, bpp);
    if (rc == 0)
        g_gpu.backbuffer = &sw_backbuffer;
    return rc;
}
