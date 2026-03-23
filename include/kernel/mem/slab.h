#ifndef SLAB_H
#define SLAB_H

#include <stdint.h>

#define SLAB_MAGIC       0x534C4142U  /* "SLAB" */
#define SLAB_NUM_CACHES  8
#define SLAB_MAX_OBJ     2048U
#define SLAB_PAGE_SIZE   4096U

typedef struct {
    uint32_t cache_obj_size[SLAB_NUM_CACHES];
    uint32_t cache_total_objs[SLAB_NUM_CACHES];
    uint32_t cache_free_objs[SLAB_NUM_CACHES];
    uint32_t cache_page_count[SLAB_NUM_CACHES];
    uint32_t total_pages;
    uint32_t total_bytes;
} slab_stats_t;

void  slab_init(void);
void* slab_alloc(uint32_t size);
void  slab_free(void* ptr);
int   slab_owns(void* ptr);
void  slab_get_stats(slab_stats_t* stats);

#endif
