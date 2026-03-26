#ifndef GUI_RENDERER_H
#define GUI_RENDERER_H

#include <stdint.h>

/* ====================================================================
 *  Lyth Renderer Abstraction
 *
 *  All drawing goes through a renderer backend (software or GPU).
 *  The active renderer is set once at init; all code uses gpu_*() calls.
 *
 *  Key concepts:
 *    gpu_texture_t  — a pixel buffer (surface/texture)
 *    render target   — which texture drawing goes into (NULL = backbuffer)
 *    clip rect       — scissor; all ops are clipped to this rect
 *    quad            — batched textured rectangle for GPU submission
 * ==================================================================== */

/* ---- Texture flags ---- */
#define GPU_TEX_TARGET   (1U << 0)  /* usable as render target          */
#define GPU_TEX_STATIC   (1U << 1)  /* hint: contents rarely change     */
#define GPU_TEX_STREAM   (1U << 2)  /* hint: contents update every frame*/
#define GPU_TEX_WRAPPED  (1U << 3)  /* pixels owned externally (no free)*/

/* ---- Blend modes ---- */
#define GPU_BLEND_NONE   0  /* opaque overwrite  */
#define GPU_BLEND_ALPHA  1  /* src-over alpha     */
#define GPU_BLEND_ADD    2  /* additive           */

/* ---- Texture ---- */
typedef struct gpu_texture {
    uint32_t  id;               /* unique handle (0 = invalid)       */
    int       width, height;
    int       stride;           /* pixels per row (≥ width)          */
    uint32_t *pixels;           /* CPU-accessible pixel data (ARGB32)*/
    uint32_t  alloc_phys;       /* phys addr for freeing             */
    uint32_t  alloc_size;
    uint32_t  flags;
    uint32_t  _gpu_handle;      /* backend-private (GL tex id, etc.) */
} gpu_texture_t;

/* ---- Clip rectangle ---- */
typedef struct gpu_clip {
    int x, y, w, h;
    int active;                 /* 0 = no clip (full target)         */
} gpu_clip_t;

/* ---- Batched quad (for GPU-style submission) ---- */
typedef struct gpu_quad {
    gpu_texture_t *texture;     /* source (NULL = solid fill)        */
    int sx, sy, sw, sh;        /* source rect in texture            */
    int dx, dy, dw, dh;        /* destination rect on target        */
    uint32_t color;             /* fill color or tint                */
    uint8_t  alpha;             /* per-quad opacity (0–255)          */
    uint8_t  blend;             /* GPU_BLEND_*                       */
} gpu_quad_t;

/* ---- Renderer backend vtable ---- */
typedef struct gpu_ops {
    const char *name;           /* "software", "virtio-gpu", …       */

    /* --- lifecycle --- */
    int   (*init)(int w, int h, int bpp);
    void  (*shutdown)(void);

    /* --- texture management --- */
    gpu_texture_t* (*texture_create)(int w, int h, uint32_t flags);
    void  (*texture_destroy)(gpu_texture_t *t);
    void  (*texture_upload)(gpu_texture_t *t, int x, int y, int w, int h,
                            const uint32_t *pixels);
    void  (*texture_download)(gpu_texture_t *t, int x, int y, int w, int h,
                              uint32_t *out);

    /* --- render target --- */
    void  (*set_target)(gpu_texture_t *t);  /* NULL = backbuffer    */
    gpu_texture_t* (*get_target)(void);

    /* --- clip / scissor --- */
    void  (*set_clip)(int x, int y, int w, int h);
    void  (*clear_clip)(void);

    /* --- clear --- */
    void  (*clear)(uint32_t color);

    /* --- drawing primitives --- */
    void  (*fill_rect)(int x, int y, int w, int h, uint32_t color);
    void  (*fill_rect_alpha)(int x, int y, int w, int h,
                             uint32_t color, int alpha);
    void  (*fill_rounded)(int x, int y, int w, int h, int r, uint32_t color);
    void  (*fill_rounded_alpha)(int x, int y, int w, int h, int r,
                                uint32_t color, int alpha);
    void  (*hline)(int x, int y, int w, uint32_t color);
    void  (*putpixel)(int x, int y, uint32_t color);

    /* --- blit (opaque copy from texture) --- */
    void  (*blit)(int dx, int dy, gpu_texture_t *src,
                  int sx, int sy, int sw, int sh);
    void  (*blit_scaled)(int dx, int dy, int dw, int dh,
                         gpu_texture_t *src, int sx, int sy, int sw, int sh);

    /* --- blit with uniform alpha (window opacity) --- */
    void  (*blit_alpha)(int dx, int dy, gpu_texture_t *src,
                        int sx, int sy, int sw, int sh, int alpha);

    /* --- blit with per-pixel alpha (ARGB icon data) --- */
    void  (*blit_icon)(int dx, int dy,
                       const uint32_t *pixels, int w, int h);
    void  (*blit_icon_scaled)(int dx, int dy, int dw, int dh,
                              const uint32_t *pixels, int sw, int sh);

    /* --- text --- */
    void  (*draw_char)(int x, int y, unsigned char c,
                       uint32_t fg, uint32_t bg, int use_bg);
    void  (*draw_string)(int x, int y, const char *s,
                         uint32_t fg, uint32_t bg, int use_bg);
    void  (*draw_char_alpha)(int x, int y, unsigned char c,
                             uint32_t fg, int alpha);

    /* --- effects --- */
    void  (*box_blur)(int x, int y, int w, int h, int passes);
    void  (*shadow)(int wx, int wy, int ww, int wh,
                    int off, int spread, int alpha);

    /* --- batched quad submission --- */
    void  (*submit_quads)(const gpu_quad_t *quads, int count);

    /* --- present (flip / copy to display) --- */
    void  (*present)(int x, int y, int w, int h);
    void  (*present_full)(void);
    void  (*vsync_wait)(void);

    /* --- resize backbuffer (hot resolution change) --- */
    int   (*resize)(int new_w, int new_h);

} gpu_ops_t;

/* ---- Global renderer ---- */
typedef struct gpu_renderer {
    const gpu_ops_t *ops;
    int width, height, bpp;
    gpu_texture_t *backbuffer;   /* main compositing target           */
} gpu_renderer_t;

/* The single global renderer instance (defined in the active backend) */
extern gpu_renderer_t g_gpu;

/* ---- Convenience inline wrappers ---- */

static inline gpu_texture_t* gpu_texture_create(int w, int h, uint32_t flags) {
    return g_gpu.ops->texture_create(w, h, flags);
}
static inline void gpu_texture_destroy(gpu_texture_t *t) {
    g_gpu.ops->texture_destroy(t);
}
static inline void gpu_texture_upload(gpu_texture_t *t,
                                      int x, int y, int w, int h,
                                      const uint32_t *px) {
    g_gpu.ops->texture_upload(t, x, y, w, h, px);
}

static inline void gpu_set_target(gpu_texture_t *t) {
    g_gpu.ops->set_target(t);
}
static inline gpu_texture_t* gpu_get_target(void) {
    return g_gpu.ops->get_target();
}

static inline void gpu_set_clip(int x, int y, int w, int h) {
    g_gpu.ops->set_clip(x, y, w, h);
}
static inline void gpu_clear_clip(void) {
    g_gpu.ops->clear_clip();
}

static inline void gpu_clear(uint32_t color) {
    g_gpu.ops->clear(color);
}
static inline void gpu_fill_rect(int x, int y, int w, int h, uint32_t c) {
    g_gpu.ops->fill_rect(x, y, w, h, c);
}
static inline void gpu_fill_rect_alpha(int x, int y, int w, int h,
                                       uint32_t c, int a) {
    g_gpu.ops->fill_rect_alpha(x, y, w, h, c, a);
}
static inline void gpu_fill_rounded(int x, int y, int w, int h,
                                    int r, uint32_t c) {
    g_gpu.ops->fill_rounded(x, y, w, h, r, c);
}
static inline void gpu_fill_rounded_alpha(int x, int y, int w, int h,
                                          int r, uint32_t c, int a) {
    g_gpu.ops->fill_rounded_alpha(x, y, w, h, r, c, a);
}
static inline void gpu_hline(int x, int y, int w, uint32_t c) {
    g_gpu.ops->hline(x, y, w, c);
}
static inline void gpu_putpixel(int x, int y, uint32_t c) {
    g_gpu.ops->putpixel(x, y, c);
}

static inline void gpu_blit(int dx, int dy, gpu_texture_t *src,
                             int sx, int sy, int sw, int sh) {
    g_gpu.ops->blit(dx, dy, src, sx, sy, sw, sh);
}
static inline void gpu_blit_scaled(int dx, int dy, int dw, int dh,
                                   gpu_texture_t *src,
                                   int sx, int sy, int sw, int sh) {
    g_gpu.ops->blit_scaled(dx, dy, dw, dh, src, sx, sy, sw, sh);
}
static inline void gpu_blit_alpha(int dx, int dy, gpu_texture_t *src,
                                  int sx, int sy, int sw, int sh, int a) {
    g_gpu.ops->blit_alpha(dx, dy, src, sx, sy, sw, sh, a);
}

static inline void gpu_blit_icon(int dx, int dy,
                                 const uint32_t *pixels, int w, int h) {
    g_gpu.ops->blit_icon(dx, dy, pixels, w, h);
}
static inline void gpu_blit_icon_scaled(int dx, int dy, int dw, int dh,
                                        const uint32_t *px, int sw, int sh) {
    g_gpu.ops->blit_icon_scaled(dx, dy, dw, dh, px, sw, sh);
}

static inline void gpu_draw_char(int x, int y, unsigned char c,
                                 uint32_t fg, uint32_t bg, int use_bg) {
    g_gpu.ops->draw_char(x, y, c, fg, bg, use_bg);
}
static inline void gpu_draw_string(int x, int y, const char *s,
                                   uint32_t fg, uint32_t bg, int use_bg) {
    g_gpu.ops->draw_string(x, y, s, fg, bg, use_bg);
}
static inline void gpu_draw_char_alpha(int x, int y, unsigned char c,
                                       uint32_t fg, int alpha) {
    g_gpu.ops->draw_char_alpha(x, y, c, fg, alpha);
}

static inline void gpu_box_blur(int x, int y, int w, int h, int passes) {
    g_gpu.ops->box_blur(x, y, w, h, passes);
}
static inline void gpu_shadow(int wx, int wy, int ww, int wh,
                              int off, int spread, int alpha) {
    g_gpu.ops->shadow(wx, wy, ww, wh, off, spread, alpha);
}

static inline void gpu_submit_quads(const gpu_quad_t *q, int count) {
    g_gpu.ops->submit_quads(q, count);
}

static inline void gpu_present(int x, int y, int w, int h) {
    g_gpu.ops->present(x, y, w, h);
}
static inline void gpu_present_full(void) {
    g_gpu.ops->present_full();
}
static inline void gpu_vsync_wait(void) {
    g_gpu.ops->vsync_wait();
}

/* ---- Backend registration ---- */
int  gpu_init_sw(int w, int h, int bpp);      /* software backend    */
int  gpu_init_virtio(int w, int h, int bpp);  /* virtio-gpu backend  */

/* SW ops table (usable by other backends for delegation) */
extern const gpu_ops_t sw_ops;

/* ---- Surface ↔ texture bridge ---- */
/* Wrap an existing pixel buffer as a gpu_texture (no allocation).
 * The wrapped texture is stored in the caller-provided storage.
 * Pixels are NOT owned — caller must not destroy the texture. */
static inline void gpu_texture_wrap(gpu_texture_t *out,
                                    uint32_t *pixels, int w, int h, int stride) {
    out->id          = 0;  /* wrapped textures have no managed id */
    out->width       = w;
    out->height      = h;
    out->stride      = stride;
    out->pixels      = pixels;
    out->alloc_phys  = 0;
    out->alloc_size  = 0;
    out->flags       = GPU_TEX_WRAPPED;
    out->_gpu_handle = 0;
}

#endif /* GUI_RENDERER_H */
