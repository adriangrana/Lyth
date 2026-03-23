#include "heap.h"
#include "slab.h"
#include "spinlock.h"
#include "physmem.h"
#include "string.h"

/*
 * Dynamic kernel heap.
 *
 * Strategy: first-fit free-list with splitting and coalescing.
 * Memory comes from physmem_alloc_region() (identity-mapped).
 * Starts with HEAP_INITIAL_SIZE and grows in HEAP_GROW_SIZE chunks.
 * Maximum HEAP_MAX_REGIONS expansion regions.
 */

#define HEAP_INITIAL_SIZE  (4 * 1024 * 1024)   /* 4 MB */
#define HEAP_GROW_SIZE     (2 * 1024 * 1024)    /* 2 MB per expansion */
#define HEAP_MAX_REGIONS   32
#define HEAP_ALIGNMENT     16
#define HEAP_BLOCK_MAGIC   0x4B484550           /* "KHEP" */

typedef struct heap_block {
    unsigned int magic;
    unsigned int size;
    int free;
    struct heap_block* next;
} heap_block_t;

typedef struct {
    uint32_t phys_addr;
    uint32_t size;
} heap_region_t;

static heap_block_t* heap_head = 0;
static spinlock_t heap_lock = SPINLOCK_INIT;
static heap_region_t regions[HEAP_MAX_REGIONS];
static int region_count = 0;
static unsigned int heap_total_size = 0;

static unsigned int align_up(unsigned int size) {
    unsigned int mask = HEAP_ALIGNMENT - 1;
    return (size + mask) & ~mask;
}

static void split_block(heap_block_t* block, unsigned int size) {
    heap_block_t* next_block;
    unsigned int remaining;
    unsigned int min_split = sizeof(heap_block_t) + HEAP_ALIGNMENT;

    if (block == 0 || block->size <= size + min_split) {
        return;
    }

    remaining = block->size - size - sizeof(heap_block_t);
    next_block = (heap_block_t*)((unsigned char*)(block + 1) + size);
    next_block->magic = HEAP_BLOCK_MAGIC;
    next_block->size = remaining;
    next_block->free = 1;
    next_block->next = block->next;

    block->size = size;
    block->next = next_block;
}

static void coalesce_blocks(void) {
    heap_block_t* block = heap_head;

    while (block != 0 && block->next != 0) {
        if (block->free && block->next->free) {
            block->size += sizeof(heap_block_t) + block->next->size;
            block->next = block->next->next;
            continue;
        }
        block = block->next;
    }
}

/* Add a new memory region to the heap free list */
static int heap_add_region(uint32_t phys, uint32_t size) {
    heap_block_t* new_block;
    heap_block_t* last;

    if (region_count >= HEAP_MAX_REGIONS || size <= sizeof(heap_block_t)) {
        return 0;
    }

    regions[region_count].phys_addr = phys;
    regions[region_count].size = size;
    region_count++;

    new_block = (heap_block_t*)(uint32_t)phys;
    new_block->magic = HEAP_BLOCK_MAGIC;
    new_block->size = size - sizeof(heap_block_t);
    new_block->free = 1;
    new_block->next = 0;

    heap_total_size += size;

    if (!heap_head) {
        heap_head = new_block;
        return 1;
    }

    /* Append to end of list */
    last = heap_head;
    while (last->next != 0) {
        last = last->next;
    }
    last->next = new_block;

    coalesce_blocks();
    return 1;
}

/* Try to grow the heap by allocating more physical memory */
static int heap_grow(void) {
    uint32_t phys = physmem_alloc_region(HEAP_GROW_SIZE, 4096);
    if (!phys) return 0;
    return heap_add_region(phys, HEAP_GROW_SIZE);
}

void heap_init(void) {
    uint32_t phys;

    heap_head = 0;
    region_count = 0;
    heap_total_size = 0;

    phys = physmem_alloc_region(HEAP_INITIAL_SIZE, 4096);
    if (!phys) {
        return;
    }

    heap_add_region(phys, HEAP_INITIAL_SIZE);
    slab_init();
}

void* kmalloc(unsigned int size) {
    heap_block_t* block;
    void* result = 0;

    if (size == 0 || heap_head == 0) {
        return 0;
    }

    /* Small allocations go through the slab allocator */
    if (size <= SLAB_MAX_OBJ) {
        result = slab_alloc(size);
        if (result)
            return result;
    }

    size = align_up(size);

    uint32_t flags = spinlock_acquire_irqsave(&heap_lock);

    /* First-fit search */
    block = heap_head;
    while (block != 0) {
        if (block->free && block->size >= size) {
            split_block(block, size);
            block->free = 0;
            result = (void*)(block + 1);
            break;
        }
        block = block->next;
    }

    /* Try to grow if allocation failed */
    if (!result) {
        if (heap_grow()) {
            block = heap_head;
            while (block != 0) {
                if (block->free && block->size >= size) {
                    split_block(block, size);
                    block->free = 0;
                    result = (void*)(block + 1);
                    break;
                }
                block = block->next;
            }
        }
    }

    spinlock_release_irqrestore(&heap_lock, flags);
    return result;
}

void kfree(void* ptr) {
    heap_block_t* block;

    if (ptr == 0) {
        return;
    }

    /* If it belongs to a slab page, free through slab */
    if (slab_owns(ptr)) {
        slab_free(ptr);
        return;
    }

    uint32_t flags = spinlock_acquire_irqsave(&heap_lock);
    block = ((heap_block_t*)ptr) - 1;

    if (block->magic != HEAP_BLOCK_MAGIC) {
        /* Corrupt or invalid pointer — do nothing */
        spinlock_release_irqrestore(&heap_lock, flags);
        return;
    }

    block->free = 1;
    coalesce_blocks();
    spinlock_release_irqrestore(&heap_lock, flags);
}

void heap_get_stats(heap_stats_t* stats) {
    heap_block_t* block;
    unsigned int used = 0;
    unsigned int free_size = 0;
    unsigned int blocks = 0;
    unsigned int free_blocks = 0;

    if (stats == 0) {
        return;
    }

    uint32_t flags = spinlock_acquire_irqsave(&heap_lock);
    block = heap_head;
    while (block != 0) {
        blocks++;
        if (block->free) {
            free_blocks++;
            free_size += block->size;
        } else {
            used += block->size;
        }
        block = block->next;
    }
    spinlock_release_irqrestore(&heap_lock, flags);

    stats->total_size = heap_total_size;
    stats->used_size = used;
    stats->free_size = free_size;
    stats->block_count = blocks;
    stats->free_block_count = free_blocks;
}
