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
    int       channels;
    uint32_t *pixels;
    uint32_t  _alloc_phys;
    uint32_t  _alloc_size;
} png_image_t;

int  png_load(const uint8_t *buf, size_t len, png_image_t *img);
void png_free(png_image_t *img);

#endif /* LIB_PNG_H */
