#ifndef PHYSMEM_H
#define PHYSMEM_H

#include <stdint.h>
#include "multiboot.h"

#define PHYSMEM_FRAME_SIZE 4096U

void physmem_init(multiboot_info_t* mbi);
uint32_t physmem_alloc_frame(void);
void physmem_free_frame(uint32_t physical_address);
uint32_t physmem_alloc_region(uint32_t size, uint32_t alignment);
void physmem_free_region(uint32_t start, uint32_t length);
void physmem_reserve_region(uint32_t start, uint32_t length);
uint32_t physmem_total_bytes(void);
uint32_t physmem_free_bytes(void);
uint32_t physmem_used_bytes(void);
uint32_t physmem_frame_count(void);
uint32_t physmem_highest_address(void);

#endif