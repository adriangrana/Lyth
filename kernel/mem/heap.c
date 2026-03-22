#include "heap.h"

#define HEAP_SIZE (256 * 1024)
#define HEAP_ALIGNMENT 8

typedef struct heap_block {
    unsigned int size;
    int free;
    struct heap_block* next;
} heap_block_t;

static unsigned char heap_area[HEAP_SIZE];
static heap_block_t* heap_head = 0;

static unsigned int align_up(unsigned int size) {
    unsigned int mask = HEAP_ALIGNMENT - 1;
    return (size + mask) & ~mask;
}

static void split_block(heap_block_t* block, unsigned int size) {
    heap_block_t* next_block;
    unsigned int remaining;

    if (block == 0 || block->size <= size + sizeof(heap_block_t) + HEAP_ALIGNMENT) {
        return;
    }

    remaining = block->size - size - sizeof(heap_block_t);
    next_block = (heap_block_t*)((unsigned char*)(block + 1) + size);
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

void heap_init(void) {
    heap_head = (heap_block_t*)heap_area;
    heap_head->size = HEAP_SIZE - sizeof(heap_block_t);
    heap_head->free = 1;
    heap_head->next = 0;
}

void* kmalloc(unsigned int size) {
    heap_block_t* block;

    if (size == 0 || heap_head == 0) {
        return 0;
    }

    size = align_up(size);
    block = heap_head;

    while (block != 0) {
        if (block->free && block->size >= size) {
            split_block(block, size);
            block->free = 0;
            return (void*)(block + 1);
        }

        block = block->next;
    }

    return 0;
}

void kfree(void* ptr) {
    heap_block_t* block;

    if (ptr == 0) {
        return;
    }

    block = ((heap_block_t*)ptr) - 1;
    block->free = 1;
    coalesce_blocks();
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

    stats->total_size = HEAP_SIZE;
    stats->used_size = used;
    stats->free_size = free_size;
    stats->block_count = blocks;
    stats->free_block_count = free_blocks;
}
