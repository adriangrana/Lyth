#include "slab.h"
#include "physmem.h"
#include "spinlock.h"

/*
 * Slab allocator for small kernel objects.
 *
 * Provides power-of-2 caches (16 .. 2048 bytes).  Each cache is backed by
 * 4 KB pages obtained from physmem_alloc_frame().  A control header lives
 * at the beginning of every page; the remainder is divided into equal-size
 * objects with an embedded free-list pointer.
 *
 * Detection:  kfree() aligns the pointer down to the page boundary and
 * checks for SLAB_MAGIC to decide whether the allocation belongs to the
 * slab layer or to the first-fit heap.
 */

/* Per-page control header – sits at offset 0 of every slab page */
typedef struct slab_page {
    uint32_t          magic;        /* SLAB_MAGIC                           */
    struct slab_page* next;         /* next page in partial / full list     */
    uint16_t          obj_size;     /* usable object size (power of 2)      */
    uint16_t          capacity;     /* total objects that fit in the page   */
    uint16_t          free_count;   /* objects currently free               */
    uint8_t           cache_idx;    /* index into slab_caches[]             */
    uint8_t           _pad;
    void*             free_list;    /* head of embedded free-list           */
} slab_page_t;

/* One size-class cache */
typedef struct {
    slab_page_t* partial;    /* pages with ≥1 free objects */
    slab_page_t* full;       /* pages with 0 free objects  */
    uint16_t     obj_size;
    spinlock_t   lock;
} slab_cache_t;

static const uint16_t cache_sizes[SLAB_NUM_CACHES] = {
    16, 32, 64, 128, 256, 512, 1024, 2048
};

static slab_cache_t slab_caches[SLAB_NUM_CACHES];
static int slab_ready = 0;

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static int cache_index_for_size(uint32_t size) {
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        if (size <= cache_sizes[i])
            return i;
    }
    return -1;
}

static uint32_t header_size_aligned(uint16_t obj_size) {
    uint32_t hdr = (uint32_t)sizeof(slab_page_t);
    uint32_t mask = (uint32_t)obj_size - 1U;
    return (hdr + mask) & ~mask;
}

/* Allocate a fresh 4 KB page and carve it up for cache at index `ci` */
static slab_page_t* slab_page_create(int ci) {
    uint32_t phys = physmem_alloc_frame();
    slab_page_t* page;
    uint32_t hdr_end;
    uint32_t usable;
    uint16_t cap;
    uint8_t* base;

    if (phys == 0)
        return 0;

    page = (slab_page_t*)(uintptr_t)phys;

    hdr_end = header_size_aligned(cache_sizes[ci]);
    usable  = SLAB_PAGE_SIZE - hdr_end;
    cap     = (uint16_t)(usable / cache_sizes[ci]);

    page->magic      = SLAB_MAGIC;
    page->next       = 0;
    page->obj_size   = cache_sizes[ci];
    page->capacity   = cap;
    page->free_count = cap;
    page->cache_idx  = (uint8_t)ci;
    page->_pad       = 0;

    /* Build embedded free-list (each free object stores a pointer to next) */
    base = (uint8_t*)page + hdr_end;
    page->free_list = 0;
    for (uint16_t i = 0; i < cap; i++) {
        void** obj = (void**)(base + (uint32_t)i * cache_sizes[ci]);
        *obj = page->free_list;
        page->free_list = obj;
    }

    return page;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void slab_init(void) {
    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        slab_caches[i].partial  = 0;
        slab_caches[i].full     = 0;
        slab_caches[i].obj_size = cache_sizes[i];
        slab_caches[i].lock     = (spinlock_t)SPINLOCK_INIT;
    }
    slab_ready = 1;
}

void* slab_alloc(uint32_t size) {
    int ci;
    slab_cache_t* cache;
    slab_page_t*  page;
    void*         obj;

    if (!slab_ready || size == 0 || size > SLAB_MAX_OBJ)
        return 0;

    ci = cache_index_for_size(size);
    if (ci < 0)
        return 0;

    cache = &slab_caches[ci];
    uint32_t flags = spinlock_acquire_irqsave(&cache->lock);

    /* Try an existing partial page first */
    page = cache->partial;
    if (!page) {
        /* No partial pages – allocate a fresh slab */
        page = slab_page_create(ci);
        if (!page) {
            spinlock_release_irqrestore(&cache->lock, flags);
            return 0;
        }
        cache->partial = page;
    }

    /* Pop one object from the free-list */
    obj = page->free_list;
    page->free_list = *(void**)obj;
    page->free_count--;

    /* If page is now full, move it to the full list */
    if (page->free_count == 0) {
        cache->partial = page->next;
        page->next = cache->full;
        cache->full = page;
    }

    spinlock_release_irqrestore(&cache->lock, flags);
    return obj;
}

void slab_free(void* ptr) {
    slab_page_t*  page;
    slab_cache_t* cache;
    int was_full;

    if (!ptr)
        return;

    page = (slab_page_t*)((uint32_t)(uintptr_t)ptr & ~(SLAB_PAGE_SIZE - 1U));
    if (page->magic != SLAB_MAGIC)
        return;

    cache = &slab_caches[page->cache_idx];
    uint32_t flags = spinlock_acquire_irqsave(&cache->lock);

    was_full = (page->free_count == 0);

    /* Push object back onto page free-list */
    *(void**)ptr = page->free_list;
    page->free_list = ptr;
    page->free_count++;

    /* Move page from full → partial if it was full */
    if (was_full) {
        /* Unlink from full list */
        slab_page_t** pp = &cache->full;
        while (*pp && *pp != page)
            pp = &(*pp)->next;
        if (*pp)
            *pp = page->next;

        /* Insert into partial list */
        page->next = cache->partial;
        cache->partial = page;
    }

    /* Optionally: if page is 100% free we could release it back to physmem.
     * For now keep it cached to avoid physmem churn. */

    spinlock_release_irqrestore(&cache->lock, flags);
}

int slab_owns(void* ptr) {
    slab_page_t* page;

    if (!ptr)
        return 0;

    page = (slab_page_t*)((uint32_t)(uintptr_t)ptr & ~(SLAB_PAGE_SIZE - 1U));
    return (page->magic == SLAB_MAGIC) ? 1 : 0;
}

void slab_get_stats(slab_stats_t* stats) {
    slab_page_t* page;

    if (!stats)
        return;

    stats->total_pages = 0;
    stats->total_bytes = 0;

    for (int i = 0; i < SLAB_NUM_CACHES; i++) {
        uint32_t total = 0, free_o = 0, pages = 0;

        uint32_t flags = spinlock_acquire_irqsave(&slab_caches[i].lock);

        for (page = slab_caches[i].partial; page; page = page->next) {
            pages++;
            total  += page->capacity;
            free_o += page->free_count;
        }
        for (page = slab_caches[i].full; page; page = page->next) {
            pages++;
            total  += page->capacity;
            /* free_count == 0 for full pages */
        }

        spinlock_release_irqrestore(&slab_caches[i].lock, flags);

        stats->cache_obj_size[i]   = cache_sizes[i];
        stats->cache_total_objs[i] = total;
        stats->cache_free_objs[i]  = free_o;
        stats->cache_page_count[i] = pages;
        stats->total_pages += pages;
    }

    stats->total_bytes = stats->total_pages * SLAB_PAGE_SIZE;
}
