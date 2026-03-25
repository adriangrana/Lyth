/*
 * PNG decoder — public API
 * Decodes 8-bit RGB/RGBA PNGs from memory to 0x00RRGGBB pixel buffers.
 * Pixel memory is allocated via physmem; use png_free() to release.
 */
#ifndef LIB_PNG_H
#define LIB_PNG_H

#include <stdint.h>
#include "string.h"

typedef struct {
    int       width, height;
    int       channels;       /* always 4 after decode (RGBA → 0x00RRGGBB) */
    uint32_t *pixels;         /* row-major, w*h uint32_t values */
    uint32_t  _alloc_phys;    /* internal: physmem base for free */
    uint32_t  _alloc_size;    /* internal: physmem size for free */
} png_image_t;

/* Decode PNG from (buf, len).  Returns 0 on success, -1 on error. */
int  png_load(const uint8_t *buf, size_t len, png_image_t *img);

/* Free pixel buffer allocated by png_load. */
void png_free(png_image_t *img);

#endif /* LIB_PNG_H */
