#ifndef HEAP_H
#define HEAP_H

typedef struct {
    unsigned int total_size;
    unsigned int used_size;
    unsigned int free_size;
    unsigned int block_count;
    unsigned int free_block_count;
} heap_stats_t;

void heap_init(void);
void* kmalloc(unsigned int size);
void kfree(void* ptr);
void heap_get_stats(heap_stats_t* stats);

#endif
